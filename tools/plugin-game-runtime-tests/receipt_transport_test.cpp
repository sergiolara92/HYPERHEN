/* SPDX-License-Identifier: GPL-3.0-or-later
 * Execute production R50 coordinator with filesystem receipts and fake
 * console primitives. Actual callback execution is tested separately. */
#include <onion/sprx_loader.hpp>
#include <onion/plugin_staging.hpp>
#include <onion/fs.h>
#include <onion/pt.h>
#include <onion/ucred.h>
#include <ps5/kernel.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
extern "C" {
#include "game-runtime/game-gate.h"
#include "game-runtime/game-inject.h"
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"receipt check %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
using namespace onion::sprx;
namespace {
GameIdentity game;
bool valid=true,stage_ok=true,writes_ok=true,metadata_change=false,freeze=false,attach_fail=false,detach_fail=false;
int created=0,create_error=0,metadata_reads=0,injected=0,attach_calls=0,fail_attach_call=0,detach_calls=0,fail_detach_call=0;
unsigned final_state=TAC_MODULE_STARTED;
uintptr_t auth=42;
TacGameGate *active=nullptr;
std::string hash(64,'a');
std::vector<void*> allocations;
void fresh(){static int seq=0;++seq;game={70000+seq,200000+seq,0,"CUSA12345",1,1};
 valid=stage_ok=writes_ok=true;metadata_change=freeze=attach_fail=detach_fail=false;
 created=create_error=metadata_reads=injected=attach_calls=fail_attach_call=detach_calls=fail_detach_call=0;final_state=TAC_MODULE_STARTED;active=nullptr;hash.assign(64,'a');CHECK(auth==42);}
LoadOptions options(){LoadOptions o;o.timeout_ms=8;o.poll_ms=1;o.game_version="01.00";return o;}
void finish(){
 if(!active||freeze)return;
 for(auto &slot:active->modules)if(slot.enabled&&slot.state==TAC_MODULE_PENDING){
  ++created;slot.phase=TAC_MODULE_PHASE_THREAD;slot.threadResult=create_error;
  if(create_error){slot.state=TAC_MODULE_REJECTED;return;}
  slot.handle=7;slot.startResult=final_state==TAC_MODULE_STARTED?0:-3;slot.state=final_state;slot.phase=TAC_MODULE_PHASE_COMPLETE;return;
 }
}
}
namespace onion::sprx {
bool capture_game(pid_t pid,GameIdentity &out)noexcept{if(!valid||pid!=game.pid)return false;out=game;return true;}
bool same_game(const GameIdentity &id)noexcept{return valid&&id.pid==game.pid&&id.seconds==game.seconds&&id.title==game.title;}
bool game_version(const GameIdentity &id,std::string &version,std::string &digest)noexcept{++metadata_reads;version="01.00";digest.assign(64,metadata_change&&metadata_reads>1?'b':'a');return same_game(id);}
bool stage_module(const GameIdentity&,std::string_view,uint32_t,std::string &path,std::string &digest,std::string&)noexcept{path="/hyperhen-modules/"+hash+".prx";digest=hash;return stage_ok;}
}
extern "C" {
uint32_t hhp_ps5_process_sdk(const void*,size_t){return 0x03000000;}
uintptr_t set_ucred_to_ptrace(){auto old=auth;auth=99;return old;}
int set_proc_authid(pid_t,uintptr_t value){auth=value;return 0;}
int hhp_plugin_attach_owned(pid_t,int *owned){++attach_calls;*owned=1;return attach_fail||attach_calls==fail_attach_call?-1:0;}
int pt_detach(pid_t,int){++detach_calls;return detach_fail||detach_calls==fail_detach_call?-1:0;}
intptr_t pt_mmap(pid_t,intptr_t,size_t bytes,int,int,int,off_t){void *p=std::calloc(1,bytes);CHECK(p);allocations.push_back(p);return(intptr_t)p;}
int pt_munmap(pid_t,intptr_t,size_t){return 0;}
int pt_copyin(pid_t,const void*s,intptr_t d,size_t n){memcpy((void*)d,s,n);return 0;}
int pt_copyout(pid_t,intptr_t s,void*d,size_t n){memcpy(d,(void*)s,n);return 0;}
long pt_syscall(pid_t,int number,...){CHECK(number==598);va_list ap;va_start(ap,number);auto a=va_arg(ap,intptr_t);auto b=va_arg(ap,intptr_t);va_end(ap);static unsigned char p[32];uint64_t addr=(uintptr_t)p,size=32;memcpy((void*)a,&addr,8);memcpy((void*)b,&size,8);return 0;}
int tac_game_inject(pid_t,uint64_t cookie,int(*identity)(void*),void*arg,TacGameInjection*out){
 CHECK(identity(arg));++injected;active=(TacGameGate*)std::calloc(1,sizeof(TacGameGate));CHECK(active);allocations.push_back(active);
 active->magic=TAC_GAME_GATE_MAGIC;active->cookie=cookie;active->version=TAC_GAME_GATE_VERSION;active->bytes=sizeof(*active);active->enabled=1;
 active->loadStartModule=1001;active->threadCreate=1002;active->threadDetach=1003;active->dlsym=1004;
 *out={};out->installed=out->resumed=1;out->cookie=cookie;out->control=out->arena=(intptr_t)active;return 0;
}
int kernel_proc_copyout(pid_t,intptr_t s,void*d,size_t n){finish();memcpy(d,(void*)s,n);return 0;}
int onion_proc_copyout(pid_t,uint64_t s,void*d,size_t n){finish();memcpy(d,(void*)s,n);return 0;}
int sysctl(const int*,unsigned,void*out,size_t*size,const void*,size_t){CHECK(*size==sizeof(timeval));timeval boot{123,456};memcpy(out,&boot,sizeof(boot));return 0;}
bool write_file_atomic(const char*path,const void*data,size_t bytes){if(!writes_ok)return false;std::string temp=std::string(path)+".tmp";FILE*f=fopen(temp.c_str(),"wb");if(!f)return false;bool ok=fwrite(data,1,bytes,f)==bytes;ok=fclose(f)==0&&ok;return ok&&rename(temp.c_str(),path)==0;}
}
int main(){
 PtraceSprxRuntime runtime;ModuleInfo module;
 fresh();CHECK(runtime.load(game.pid,"/one.prx","one.prx",options()).succeeded());CHECK(created==1&&injected==1&&auth==42);
 CHECK(runtime.find_loaded(game.pid,"one.prx",&module));CHECK(runtime.load(game.pid,"/alias.prx","alias.prx",options()).status==LoadStatus::AlreadyLoaded&&created==1);
 fresh();freeze=true;CHECK(runtime.load(game.pid,"/pending.prx","pending.prx",options()).status==LoadStatus::Pending);
 CHECK(!runtime.find_loaded(game.pid,"pending.prx",&module));CHECK(runtime.load(game.pid,"/pending.prx","pending.prx",options()).status==LoadStatus::Pending&&created==0&&injected==1);
 freeze=false;CHECK(runtime.find_loaded(game.pid,"pending.prx",&module)&&created==1);CHECK(runtime.load(game.pid,"/pending.prx","pending.prx",options()).status==LoadStatus::AlreadyLoaded);
 fresh();freeze=true;CHECK(runtime.load(game.pid,"/unknown.prx","unknown.prx",options()).status==LoadStatus::Pending);
 active->magic=0;CHECK(runtime.load(game.pid,"/unknown.prx","unknown.prx",options()).status==LoadStatus::RemoteError&&created==0&&injected==1);
 active->magic=TAC_GAME_GATE_MAGIC;freeze=false;CHECK(runtime.find_loaded(game.pid,"unknown.prx",&module)&&created==1);
 fresh();final_state=TAC_MODULE_FAILED;CHECK(!runtime.load(game.pid,"/failed.prx","failed.prx",options()).succeeded());CHECK(!runtime.load(game.pid,"/failed.prx","failed.prx",options()).succeeded()&&created==1);
 // Stale alias A must not erase a newer successful shared-content receipt B.
 fresh();create_error=5;CHECK(runtime.load(game.pid,"/A.prx","A.prx",options()).status==LoadStatus::ThreadCreateFailed);
 create_error=0;CHECK(runtime.load(game.pid,"/B.prx","B.prx",options()).succeeded());CHECK(created==2);
 CHECK(runtime.load(game.pid,"/A.prx","A.prx",options()).status==LoadStatus::AlreadyLoaded&&created==2);
 fresh();metadata_change=true;CHECK(runtime.load(game.pid,"/meta.prx","meta.prx",options()).status==LoadStatus::PolicyDenied&&created==0&&auth==42);
 metadata_change=false;CHECK(runtime.load(game.pid,"/meta.prx","meta.prx",options()).succeeded()&&created==1);
 fresh();writes_ok=false;CHECK(!runtime.load(game.pid,"/write.prx","write.prx",options()).succeeded()&&created==0&&auth==42);writes_ok=true;CHECK(runtime.load(game.pid,"/write.prx","write.prx",options()).succeeded());
 fresh();attach_fail=detach_fail=true;auto failed=runtime.load(game.pid,"/attach.prx","attach.prx",options());CHECK(failed.status==LoadStatus::AttachFailed&&!failed.cleanup_ok&&created==0&&auth==42);
 fresh();fail_attach_call=2;fail_detach_call=2;failed=runtime.load(game.pid,"/resume.prx","resume.prx",options());CHECK(failed.status==LoadStatus::AttachFailed&&!failed.cleanup_ok&&created==0&&auth==42&&attach_calls==2);
 fresh();metadata_change=true;fail_detach_call=2;failed=runtime.load(game.pid,"/reject.prx","reject.prx",options());CHECK(failed.status==LoadStatus::AttachFailed&&!failed.cleanup_ok&&failed.underlying_status==LoadStatus::PolicyDenied&&created==0&&auth==42);
 fresh();CHECK(runtime.load(game.pid,"/first.prx","first.prx",options()).succeeded());game.seconds++;CHECK(!runtime.find_loaded(game.pid,"first.prx",&module));CHECK(runtime.load(game.pid,"/first.prx","first.prx",options()).succeeded()&&created==2&&injected==2);
 fresh();for(unsigned i=0;i<8;i++){hash.assign(64,'a'+i);auto name=std::to_string(i)+".prx";CHECK(runtime.load(game.pid,"/item.prx",name,options()).succeeded());}CHECK(injected==1&&created==8);hash.assign(64,'9');CHECK(!runtime.load(game.pid,"/ninth.prx","ninth.prx",options()).succeeded()&&created==8);
 fresh();CHECK(mkdir(HYPERHEN_PLUGIN_LEGACY_ROOT,0700)==0||errno==EEXIST);char legacy[512];snprintf(legacy,sizeof(legacy),"%s/%d-%lld-%lld-old.state",HYPERHEN_PLUGIN_LEGACY_ROOT,game.pid,(long long)game.seconds,(long long)game.microseconds);FILE *f=fopen(legacy,"wb");CHECK(f);fputs("old live attempt",f);fclose(f);
 CHECK(runtime.load(game.pid,"/legacy.prx","legacy.prx",options()).status==LoadStatus::PolicyDenied&&created==0&&injected==0);CHECK(unlink(legacy)==0);
 for(void*p:allocations)free(p);
 puts("PASS: real R50 receipt coordinator; alias failure/success/retry; pending/late completion; no duplicate failed initialization; pre-dispatch retry; failed detach; legacy-generation guard; new game; eight slots");
}
