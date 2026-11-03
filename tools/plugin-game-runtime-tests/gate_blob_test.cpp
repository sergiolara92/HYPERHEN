// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the actual callback binary on x86-64, with real worker threads.
#include "game-gate.h"
#include "check.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/mman.h>
using Gate=void(*)(TacGameGate*,uint64_t,unsigned);
static Gate callback;
static TacGameGate control;
static std::thread thread;
static std::thread::id input_thread;
static std::mutex mutex;
static std::condition_variable changed;
static bool block_load,entered,released;
static int handle=7,start_result,create_result,symbol_mode,activation_result;
static std::atomic<int> calls,created,activated,pad_calls;
static int load(const char*path,uint64_t size,const void*args,unsigned flags,const void*opts,int*result){
  CHECK(path&&size==0&&!args&&!flags&&!opts&&result);
  CHECK(std::this_thread::get_id()!=input_thread);++calls;
  callback(&control,control.cookie,1); // Reentrant input must not schedule twice.
  if(block_load){std::unique_lock<std::mutex> lock(mutex);entered=true;changed.notify_all();CHECK(changed.wait_for(lock,std::chrono::seconds(3),[]{return released;}));}
  *result=start_result;return handle;
}
static int activate(int argc,const char **argv){CHECK(!argc&&!argv);++activated;return activation_result;}
static int symbol(int id,const char *name,void **out){
  CHECK(id==handle);*out=nullptr;
  if(symbol_mode==1||(symbol_mode==2&&!strcmp(name,"plugin_load"))){*out=(void*)activate;return 0;}return -1;
}
static int create(uintptr_t *out,const void *attr,TacGameThreadEntry entry,void *arg,const char*name){
  CHECK(!attr&&entry&&arg==&control&&!strcmp(name,"HyperHEN module"));
  if(create_result)return create_result;
  CHECK(!thread.joinable());++created;thread=std::thread([=]{entry(arg);});*out=1;return 0;
}
static int detach(uintptr_t value){CHECK(value==1);return 0;}
static void drain(){if(thread.joinable())thread.join();}
static void reset(){
  drain();control={};control.magic=TAC_GAME_GATE_MAGIC;control.cookie=12345;control.version=TAC_GAME_GATE_VERSION;
  control.bytes=sizeof(control);control.enabled=1;control.loadStartModule=(uintptr_t)load;control.threadCreate=(uintptr_t)create;
  control.threadDetach=(uintptr_t)detach;control.dlsym=(uintptr_t)symbol;
  calls=created=activated=pad_calls=0;handle=7;start_result=create_result=symbol_mode=activation_result=0;
  block_load=entered=released=false;
}
static void slot(unsigned index,bool global=false){
  auto &s=control.modules[index];s={};s.enabled=1;s.handle=-1;
  const char *prefix=global?"/mnt/sandbox/PPSA12345_000/hyperhen-modules/":TAC_GAME_MODULE_PREFIX;
  strcpy(s.path,prefix);size_t n=strlen(prefix);memset(s.path+n,'a'+index,64);strcpy(s.path+n+64,".prx");
}
static void run(){callback(&control,control.cookie,0);drain();}
static int pad(int a,int b,int c){CHECK(created==0);++pad_calls;return a+b+c;}
int main(int argc,char**argv){
  CHECK(argc==2);input_thread=std::this_thread::get_id();
  std::ifstream f(argv[1],std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(f)),{});CHECK(!bytes.empty()&&bytes.size()<4096);
  auto *code=(unsigned char*)mmap(nullptr,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(code!=MAP_FAILED);
  memcpy(code,bytes.data(),bytes.size());CHECK(mprotect(code,0x4000,PROT_READ|PROT_EXEC)==0);callback=(Gate)code;
  reset();slot(0);slot(1,true);run();CHECK(calls==1&&created==1&&control.modules[0].state==TAC_MODULE_STARTED);
  run();CHECK(calls==2&&control.modules[1].state==TAC_MODULE_STARTED);run();CHECK(calls==2);
  reset();slot(0);control.enabled=0;run();CHECK(calls==0);control.enabled=1;control.modules[0].enabled=0;run();CHECK(calls==0);
  reset();slot(0);callback(&control,0,0);callback(&control,control.cookie+1,0);callback(&control,control.cookie,2);CHECK(created==0);
  control.version++;run();CHECK(calls==0);
  reset();slot(0);start_result=-1;run();CHECK(control.modules[0].state==TAC_MODULE_FAILED&&calls==1);start_result=0;run();CHECK(calls==1);
  reset();slot(0);start_result=1;run();CHECK(control.modules[0].state==TAC_MODULE_STARTED);
  reset();slot(0);handle=-1;run();CHECK(control.modules[0].state==TAC_MODULE_FAILED);
  reset();slot(0);create_result=5;run();CHECK(calls==0&&control.modules[0].state==TAC_MODULE_REJECTED&&control.modules[0].threadResult==5);
  reset();slot(0);symbol_mode=1;run();CHECK(activated==1&&control.modules[0].state==TAC_MODULE_STARTED);run();CHECK(activated==1);
  reset();slot(0);symbol_mode=1;activation_result=7;run();CHECK(control.modules[0].state==TAC_MODULE_FAILED&&activated==1);
  reset();slot(0);symbol_mode=2;run();CHECK(control.modules[0].state==TAC_MODULE_STARTED&&activated==0);
  for(int malformed=0;malformed<5;malformed++){
    reset();slot(0,malformed>1);
    if(malformed==0)control.modules[0].path[strlen(TAC_GAME_MODULE_PREFIX)]='Z';
    if(malformed==1)control.modules[0].path[0]='X';
    if(malformed==2)control.modules[0].path[13]='!';
    if(malformed==3)memset(control.modules[0].path,'x',sizeof(control.modules[0].path));
    if(malformed==4)strcpy(control.modules[0].path,"/mnt/sandbox/PPSA12345_0000000/hyperhen-modules/");
    run();CHECK(calls==0&&control.modules[0].state==TAC_MODULE_REJECTED);
  }
  reset();slot(0);slot(1);block_load=true;
  callback(&control,control.cookie,0);
  {std::unique_lock<std::mutex> lock(mutex);CHECK(changed.wait_for(lock,std::chrono::seconds(3),[]{return entered;}));}
  CHECK(control.busy==1&&control.modules[0].phase==TAC_MODULE_PHASE_LOAD);callback(&control,control.cookie,1);CHECK(created==1);
  {std::lock_guard<std::mutex> lock(mutex);released=true;changed.notify_all();}drain();CHECK(control.busy==0);
  block_load=false;run();CHECK(calls==2);
  // Execute the real original-first import thunk and preserve the pad return.
  reset();slot(0);TacGameTrampoline thunk{};
  CHECK(!tac_game_import_trampoline((uintptr_t)pad,(uintptr_t)&control,control.cookie,(uintptr_t)callback,0,&thunk));
  auto *entry=(unsigned char*)mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(entry!=MAP_FAILED);
  memcpy(entry,thunk.bytes,thunk.size);CHECK(mprotect(entry,4096,PROT_READ|PROT_EXEC)==0);
  CHECK(((int(*)(int,int,int))entry)(2,3,7)==12);drain();CHECK(pad_calls==1&&calls==1);
  CHECK(munmap(entry,4096)==0&&munmap(code,0x4000)==0);
  puts("PASS: actual callback blob, separate real worker, blocked/reentrant input, path validation, activation and original-first thunk");
}
