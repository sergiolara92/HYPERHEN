/* SPDX-License-Identifier: GPL-3.0-or-later
 * Dedicated game-plugin transport; does not change the shared ShellUI pt.c.
 */
#include "game-syscall.h"
#include "game-syscall-core.h"
#include "memory-map.h"
#include <onion/log.h>
#include <onion/credentials_guard.h>
#include <ps5/kernel.h>
#include <sys/ptrace.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

/* The installed SDK header omits this declaration; the unchanged isolated
 * CRT exports the initialized symbol with the SDK's unsigned long ABI. */
extern unsigned long KERNEL_OFFSET_VMSPACE_P_ROOT;

/* Private read-only adaptation of ps5-payload-dev SDK crt/mdbg.c, John
 * Tornblom, GPL-3.0-or-later. ABI verified against the vendored SDK source
 * in third_party/hyperdeck-tools/ps5upload/payload/third_party/sdk-crt.
 * The installed public syscall header lacks SYS_mdbg_call; do not replace
 * the installed SDK/isolated CRT merely to add this private definition. */
enum { HHP_SYS_MDBG_CALL=573, HHP_MDBG_READ=0x12, HHP_GATE_READ_LIMIT=64 };
typedef struct {uint64_t type,command;} HhpMdbgCommand;
typedef struct {int32_t pid;uint64_t source,destination,length;} HhpMdbgRead;
typedef struct {int32_t status;uint64_t length;} HhpMdbgResult;
_Static_assert(sizeof(HhpMdbgCommand)==16,"MDBG command ABI");
_Static_assert(sizeof(HhpMdbgRead)==32&&offsetof(HhpMdbgRead,source)==8&&offsetof(HhpMdbgRead,length)==24,"MDBG read ABI");
_Static_assert(sizeof(HhpMdbgResult)==16&&offsetof(HhpMdbgResult,length)==8,"MDBG result ABI");

static _Thread_local struct {pid_t pid;int64_t seconds,microseconds;int begun;} state;
typedef struct {pid_t pid;int64_t seconds,microseconds;int used,can_resume;} HeldGeneration;
static HeldGeneration held[64];
static pthread_mutex_t held_mutex=PTHREAD_MUTEX_INITIALIZER;
static int exhausted;
static int same_generation(const HeldGeneration *h){return h->used&&h->pid==state.pid&&h->seconds==state.seconds&&h->microseconds==state.microseconds;}
void hhp_plugin_transport_begin(pid_t pid,int64_t seconds,int64_t microseconds){
  state.pid=pid;state.seconds=seconds;state.microseconds=microseconds;state.begun=1;
  pthread_mutex_lock(&held_mutex);
  for(unsigned i=0;i<64;++i)if(held[i].used&&held[i].pid==pid&&!same_generation(&held[i]))held[i].used=0;
  pthread_mutex_unlock(&held_mutex);
}
static int allowed(pid_t pid,int resume){
  if(!state.begun||state.pid!=pid)return 0;
  pthread_mutex_lock(&held_mutex);int result=!exhausted;
  for(unsigned i=0;i<64;++i)if(same_generation(&held[i])){result=resume?held[i].can_resume:0;break;}
  pthread_mutex_unlock(&held_mutex);return result;
}
static void hold_generation(int can_resume){
  pthread_mutex_lock(&held_mutex);unsigned empty=64;
  for(unsigned i=0;i<64;++i){
    if(same_generation(&held[i])){held[i].can_resume&=can_resume;pthread_mutex_unlock(&held_mutex);return;}
    if(!held[i].used&&empty==64)empty=i;
  }
  if(empty==64)exhausted=1;
  else held[empty]=(HeldGeneration){state.pid,state.seconds,state.microseconds,1,can_resume};
  pthread_mutex_unlock(&held_mutex);
}
int hhp_plugin_transport_healthy(pid_t pid){return allowed(pid,0);}
int hhp_plugin_transport_can_resume(pid_t pid){return allowed(pid,1);}
static int ptrace_readbytes(void *c,pid_t pid,intptr_t at,void *p,size_t n){
  (void)c;struct ptrace_io_desc io={.piod_op=PIOD_READ_D,.piod_offs=(void*)at,.piod_addr=p,.piod_len=n};
  errno=0;int result=(int)syscall(SYS_ptrace,PT_IO,pid,&io,0),error=errno;
  if(result<0||io.piod_len!=n){
    if(!error)error=EIO;
    LOG_INFO("[plugins] gate-read-failed pid=%d address=%llx requested=%zu actual=%zu errno=%d",
      pid,(unsigned long long)at,n,io.piod_len,error);
    errno=error;return -1;
  }
  return 0;
}
static int restore_credentials(pid_t owner,uint64_t authid,const unsigned char caps[16]){
  for(unsigned retry=0;retry<3;++retry){
    /* Both setters and both readbacks run even when an earlier call failed. */
    int auth_result=kernel_set_ucred_authid(owner,authid);
    int caps_result=kernel_set_ucred_caps(owner,caps);
    uint64_t observed_auth=kernel_get_ucred_authid(owner);
    unsigned char observed_caps[16]={0};int read_result=kernel_get_ucred_caps(owner,observed_caps);
    if(!auth_result&&!caps_result&&!read_result&&observed_auth==authid&&!memcmp(caps,observed_caps,16))return 0;
  }
  return -1;
}
static int mdbg_readbytes(pid_t pid,intptr_t at,void *bytes,size_t length){
  if(pid<=1||!bytes||!length||length>HHP_GATE_READ_LIMIT||at<0x10000||
     (uint64_t)at>UINT64_C(0x800000000000)-length){errno=EINVAL;return -1;}
  int guard=hhp_self_credentials_lock();if(guard){errno=guard;return -1;}
  pid_t owner=getpid();unsigned char saved_caps[16]={0},elevated_caps[16],observed_caps[16]={0};
  memset(elevated_caps,0xff,sizeof elevated_caps);
  uint64_t saved_auth=0;int touched=0,result=-1,error=EIO,restored=1;size_t done=0;
  if(kernel_get_ucred_caps(owner,saved_caps))goto finish;
  saved_auth=kernel_get_ucred_authid(owner);if(!saved_auth||saved_auth==UINT64_MAX)goto finish;
  touched=1;
  if(kernel_set_ucred_authid(owner,UINT64_C(0x4800000000000006)))goto finish;
  if(kernel_set_ucred_caps(owner,elevated_caps))goto finish;
  if(kernel_get_ucred_authid(owner)!=UINT64_C(0x4800000000000006)||
     kernel_get_ucred_caps(owner,observed_caps)||memcmp(elevated_caps,observed_caps,16))goto finish;
  for(unsigned calls=0;done<length&&calls<HHP_GATE_READ_LIMIT;++calls){
    size_t count=0x1000-(((uint64_t)at+done)&0xfff);if(count>length-done)count=length-done;
    HhpMdbgCommand command={1,HHP_MDBG_READ};
    HhpMdbgRead request={pid,(uint64_t)at+done,(uintptr_t)((unsigned char*)bytes+done),count};
    HhpMdbgResult response={0};errno=0;
    long rc=syscall(HHP_SYS_MDBG_CALL,&command,&request,&response);
    if(rc<0){error=errno?errno:EIO;break;}
    if(!response.length||response.length>count||(!response.status&&response.length!=count))break;
    done+=(size_t)response.length;
    if(response.length==count&&response.status)break;
    if(done==length){result=0;error=0;}
  }
finish:
  if(touched&&restore_credentials(owner,saved_auth,saved_caps)){restored=0;result=-1;error=EIO;}
  guard=hhp_self_credentials_unlock();if(guard){restored=0;result=-1;error=guard;}
  if(!restored){
    hold_generation(0);
    LOG_INFO("[plugins] gate-credentials-restore-failed pid=%d",pid);
  }
  if(result){LOG_INFO("[plugins] gate-mdbg-read-failed pid=%d address=%llx requested=%zu actual=%zu errno=%d",pid,(unsigned long long)at,length,done,error);errno=error;}
  return result;
}
static int readbytes(void *c,pid_t pid,intptr_t at,void *p,size_t n){
  /* Only the already validated two-byte gate uses this private backend.
   * PT_IO is a diagnostic comparison; a full verified MDBG read supplies the
   * bytes used by the opcode check, as in etaHEN's primary process-read path. */
  if(n!=2){errno=EINVAL;return -1;}
  if(!hhp_plugin_transport_healthy(pid)){errno=EIO;return -1;}
  unsigned char ptrace_bytes[2]={0};
  int ptrace_result=ptrace_readbytes(c,pid,at,ptrace_bytes,n);
  int result=mdbg_readbytes(pid,at,p,n);
  if(!result){
    const unsigned char *read=p;
    LOG_INFO("[plugins] gate-read-compare pid=%d address=%llx ptrace-status=%d ptrace=%02x%02x mdbg=%02x%02x match=%d",pid,(unsigned long long)at,ptrace_result,ptrace_bytes[0],ptrace_bytes[1],read[0],read[1],!ptrace_result&&!memcmp(p,ptrace_bytes,2));
  }
  return result;
}
static int read_kernel(void *c,uint64_t at,void *bytes,size_t size){(void)c;return kernel_copyout(at,bytes,size);}
static int same_module(const TacMemoryModule *a,const TacMemoryModule *b){
  return a->object==b->object&&a->base==b->base&&a->size==b->size&&a->handle==b->handle;
}
static intptr_t resolve(void *c,pid_t pid){
  (void)c;
  /* A PS4 game's libkernel handle is not assumed to be 1 or 0x2001.
   * Resolve only named modules through the existing bounded SDK-layout reader.
   * Keep TacHen's known +0xa gate and verify it; do not scan foreign code. */
  static const char *names[]={"libkernel.sprx","libkernel_sys.sprx"};
  const TacMemoryReader reader={NULL,read_kernel};uint64_t proc=kernel_get_proc(pid);
  for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i){
    TacMemoryModule module={0},after={0};
    int found=tac_memory_module(&reader,proc,names[i],&module);
    if(found){LOG_INFO("[plugins] gate-module-unavailable pid=%d module=%s status=%d",pid,names[i],found);continue;}
    uint64_t function=kernel_dynlib_resolve(pid,module.handle,"HoLVWNanBBc");
    if(function<0x10000||function>UINT64_C(0x7fffffffffff)-12){
      LOG_INFO("[plugins] gate-export-missing pid=%d module=%s handle=%u address=%llx",pid,names[i],module.handle,(unsigned long long)function);continue;
    }
    if(module.base<0x10000||module.base>=UINT64_C(0x800000000000)||module.size<12||
       module.size>UINT64_C(0x800000000000)-module.base||function<module.base||function-module.base>module.size-12){
      LOG_INFO("[plugins] gate-export-outside-module pid=%d module=%s handle=%u address=%llx",pid,names[i],module.handle,(unsigned long long)function);continue;
    }
    uint64_t gate=function+0xa;TacMemoryMapping mappings[2]={{0}};size_t mapping_count=0;
    int mapped=tac_memory_span(&reader,proc,KERNEL_OFFSET_VMSPACE_P_ROOT,gate,2,
                              mappings,2,&mapping_count);
    if(mapped||!mapping_count||mapping_count>2){
      LOG_INFO("[plugins] gate-mapping-query-failed pid=%d module=%s handle=%u address=%llx status=%d segments=%zu",pid,names[i],module.handle,(unsigned long long)gate,mapped,mapping_count);continue;
    }
    int executable=1;
    for(size_t segment=0;segment<mapping_count;++segment){
      LOG_INFO("[plugins] gate-mapping pid=%d address=%llx segment=%zu/%zu start=%llx end=%llx protection=%u",pid,(unsigned long long)gate,segment+1,mapping_count,(unsigned long long)mappings[segment].start,(unsigned long long)mappings[segment].end,(unsigned)mappings[segment].protection);
      if(!(mappings[segment].protection&PROT_EXEC))executable=0;
    }
    if(!executable){LOG_INFO("[plugins] gate-mapping-not-executable pid=%d module=%s handle=%u address=%llx",pid,names[i],module.handle,(unsigned long long)gate);continue;}
    unsigned char opcode[2]={0};
    if(readbytes(NULL,pid,(intptr_t)gate,opcode,sizeof opcode))continue;
    if(opcode[0]!=0x0f||opcode[1]!=0x05){
      LOG_INFO("[plugins] gate-opcode-mismatch pid=%d module=%s handle=%u address=%llx bytes=%02x%02x",pid,names[i],module.handle,(unsigned long long)gate,opcode[0],opcode[1]);continue;
    }
    found=tac_memory_module(&reader,proc,names[i],&after);
    if(found||kernel_get_proc(pid)!=proc||!same_module(&module,&after)){
      LOG_INFO("[plugins] gate-module-changed pid=%d module=%s status=%d",pid,names[i],found);continue;
    }
    LOG_INFO("[plugins] gate-verified pid=%d module=%s handle=%u address=%llx",pid,names[i],module.handle,(unsigned long long)gate);
    return (intptr_t)function;
  }
  errno=ENOENT;return 0;
}
/* Only the exact Rai adapter uses this bounded native call-chain resolver.
 * The caller owns a stopped, identity-checked game generation. getpid's
 * syscall tail normalizes errno and does not preserve raw RAX/CF, so it must
 * remain confined to the existing single-step transport above.
 *
 * The observed native pthread_spin_trylock export calls a private helper
 * which jumps to a raw _umtx_op stub. Recognize every instruction, masking
 * only the two rel32 operands, and follow those operands within the same
 * executable module. No function in that chain is invoked or modified.
 * Export name/NID and both complete instruction layouts were checked in
 * the console's libkernel.sprx and libkernel_sys.sprx. Unknown layouts fail
 * before staging; there is no fixed firmware address or broad code scan. */
static int native_read_code(const TacMemoryReader *reader,uint64_t proc,pid_t pid,
                            const TacMemoryModule *module,uint64_t at,
                            unsigned char *bytes,size_t length){
  if(!length || length>70 || module->base<0x10000 ||
     module->base>=UINT64_C(0x800000000000) || module->size<length ||
     module->size>UINT64_C(0x800000000000)-module->base ||
     at<module->base || at-module->base>module->size-length){errno=ENOEXEC;return -1;}
  TacMemoryMapping mappings[2]={{0}};size_t count=0;
  if(tac_memory_span(reader,proc,KERNEL_OFFSET_VMSPACE_P_ROOT,at,length,
                     mappings,2,&count) || !count || count>2){errno=EIO;return -1;}
  for(size_t i=0;i<count;++i)if(!(mappings[i].protection&PROT_EXEC)){errno=EACCES;return -1;}
  for(size_t done=0;done<length;){
    size_t chunk=length-done;if(chunk>HHP_GATE_READ_LIMIT)chunk=HHP_GATE_READ_LIMIT;
    if(!hhp_plugin_transport_healthy(pid) || kernel_get_proc(pid)!=proc){errno=ESRCH;return -1;}
    if(mdbg_readbytes(pid,(intptr_t)(at+done),bytes+done,chunk))return -1;
    done+=chunk;
  }
  return 0;
}
intptr_t hhp_plugin_native_syscall_gate(pid_t pid){
  static const unsigned char anchor_pattern[70]={
    0x64,0x48,0x8b,0x0c,0x25,0x10,0x00,0x00,0x00,0xb8,0x16,0x00,0x00,0x00,
    0x48,0x85,0xff,0x74,0x17,0x48,0x8b,0x3f,0x48,0x85,0xff,0x74,0x0f,0x8b,
    0x09,0x31,0xc0,0xf0,0x0f,0xb1,0x0f,0xb8,0x00,0x00,0x00,0x00,0x75,0x01,
    0xc3,0xf6,0x47,0x04,0x08,0xb8,0x10,0x00,0x00,0x00,0x74,0xf4,0x55,0x48,
    0x89,0xe5,0xbe,0xff,0xff,0xff,0xff,0xe8,0x00,0x00,0x00,0x00,0x5d,0xc3};
  static const unsigned char helper_pattern[16]={
    0x41,0x89,0xf1,0xbe,0x04,0x00,0x00,0x00,0x31,0xd2,0x31,0xc9,0x45,0x31,0xc0,0xe9};
  static const unsigned char stub_pattern[13]={
    0x48,0xc7,0xc0,0xc6,0x01,0x00,0x00,0x49,0x89,0xca,0x0f,0x05,0xc3};
  static const char *names[]={"libkernel.sprx","libkernel_sys.sprx"};
  static const char nid[]="rCTGkBIHfPY"; /* pthread_spin_trylock */
  if(!hhp_plugin_transport_healthy(pid)){errno=EIO;return 0;}
  const uint64_t proc=kernel_get_proc(pid);
  if(!proc){errno=ESRCH;return 0;}
  const TacMemoryReader reader={NULL,read_kernel};
  for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i){
    TacMemoryModule module={0},after={0};
    if(tac_memory_module(&reader,proc,names[i],&module))continue;
    const uint64_t anchor=kernel_dynlib_resolve(pid,module.handle,nid);
    if(!anchor)continue;
    unsigned char anchor_code[70]={0},helper_code[20]={0},stub_code[13]={0};
    const char *phase="anchor";
    if(native_read_code(&reader,proc,pid,&module,anchor,anchor_code,sizeof anchor_code))goto failure;
    if(memcmp(anchor_code,anchor_pattern,64) || memcmp(anchor_code+68,anchor_pattern+68,2)){
      errno=ENOEXEC;goto failure;
    }
    int32_t displacement=0;memcpy(&displacement,anchor_code+64,sizeof displacement);
    const uint64_t helper=(uint64_t)((int64_t)anchor+68+(int64_t)displacement);
    phase="helper";
    if(native_read_code(&reader,proc,pid,&module,helper,helper_code,sizeof helper_code))goto failure;
    if(memcmp(helper_code,helper_pattern,sizeof helper_pattern)){errno=ENOEXEC;goto failure;}
    memcpy(&displacement,helper_code+16,sizeof displacement);
    const uint64_t stub=(uint64_t)((int64_t)helper+20+(int64_t)displacement);
    phase="stub";
    if(native_read_code(&reader,proc,pid,&module,stub,stub_code,sizeof stub_code))goto failure;
    if(memcmp(stub_code,stub_pattern,sizeof stub_pattern)){errno=ENOEXEC;goto failure;}
    /* Revalidate all three spans and the complete observed chain, including
     * relative operands, before the final module/export identity check. */
    unsigned char again[70]={0};phase="recheck";
    if(native_read_code(&reader,proc,pid,&module,anchor,again,sizeof anchor_code))goto failure;
    if(memcmp(again,anchor_code,sizeof anchor_code)){errno=ENOEXEC;goto failure;}
    if(native_read_code(&reader,proc,pid,&module,helper,again,sizeof helper_code))goto failure;
    if(memcmp(again,helper_code,sizeof helper_code)){errno=ENOEXEC;goto failure;}
    if(native_read_code(&reader,proc,pid,&module,stub,again,sizeof stub_code))goto failure;
    if(memcmp(again,stub_code,sizeof stub_code)){errno=ENOEXEC;goto failure;}
    phase="identity";
    if(kernel_get_proc(pid)!=proc || !hhp_plugin_transport_healthy(pid) ||
       tac_memory_module(&reader,proc,names[i],&after) || !same_module(&module,&after) ||
       kernel_dynlib_resolve(pid,after.handle,nid)!=anchor){errno=ESRCH;goto failure;}
    LOG_INFO("[plugins] native syscall chain verified pid=%d module=%s anchor=%llx helper=%llx gate=%llx",
      pid,names[i],(unsigned long long)anchor,(unsigned long long)helper,(unsigned long long)(stub+10));
    return (intptr_t)(stub+10);
failure:
    LOG_WARN("[plugins] native syscall chain unavailable pid=%d module=%s phase=%s errno=%d",pid,names[i],phase,errno);
    return 0;
  }
  errno=ENOENT;return 0;
}
static int getregs(void *c,pid_t pid,struct reg *r){(void)c;return pt_getregs(pid,r);}
static int setregs(void *c,pid_t pid,const struct reg *r){(void)c;return pt_setregs(pid,r);}
static int step(void *c,pid_t pid){(void)c;return syscall(SYS_ptrace,PT_STEP,pid,(void*)1,0)<0?-1:0;}
static HhpPluginTraceEvent poll(void *c,pid_t pid){
  (void)c;int status=0;pid_t got=waitpid(pid,&status,WNOHANG|WUNTRACED);
  if(!got||(got<0&&errno==EINTR))return(HhpPluginTraceEvent){HHP_TRACE_PENDING,0,0};
  if(got<0)return(HhpPluginTraceEvent){HHP_TRACE_POLL_ERROR,0,errno};
  if(WIFSTOPPED(status))return(HhpPluginTraceEvent){HHP_TRACE_STOPPED,WSTOPSIG(status),0};
  if(WIFEXITED(status)||WIFSIGNALED(status))return(HhpPluginTraceEvent){HHP_TRACE_EXITED,0,0};
  return(HhpPluginTraceEvent){HHP_TRACE_PENDING,0,0};
}
static int stop(void *c,pid_t pid){(void)c;return kill(pid,SIGSTOP);}
static int now(void *c,uint64_t *out){(void)c;struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return -1;*out=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;return 0;}
static void pause_ms(void *c,unsigned ms){(void)c;usleep(ms*1000);}
long hhp_plugin_syscall6(pid_t pid,int sysno,const uint64_t args[6]){
  if(!hhp_plugin_transport_healthy(pid)){errno=EIO;return -1;}
  const HhpPluginTraceOps ops={NULL,resolve,readbytes,getregs,setregs,step,poll,stop,now,pause_ms};
  LOG_INFO("[plugins] remote syscall begin pid=%d number=%d",pid,sysno);
  HhpPluginSyscallResult result=hhp_plugin_syscall_transaction(&ops,pid,sysno,args);
  if(result.uncertain)hold_generation(result.restored);
  LOG_INFO("[plugins] remote syscall end pid=%d number=%d status=%d restored=%d uncertain=%d result=%lld",
    pid,sysno,result.status,result.restored,result.uncertain,(long long)result.value);
  if(result.status!=HHP_SYSCALL_OK)errno=result.error?result.error:EIO;
  return result.value;
}
intptr_t hhp_plugin_mmap(pid_t pid,intptr_t address,size_t bytes,int protection,int flags,int fd,off_t offset){
  const uint64_t args[6]={(uint64_t)address,bytes,(uint64_t)protection,(uint64_t)flags,(uint64_t)(int64_t)fd,(uint64_t)offset};
  return hhp_plugin_syscall6(pid,SYS_mmap,args);
}
int hhp_plugin_mprotect(pid_t pid,intptr_t address,size_t bytes,int protection){
  const uint64_t args[6]={(uint64_t)address,bytes,(uint64_t)protection,0,0,0};
  return (int)hhp_plugin_syscall6(pid,SYS_mprotect,args);
}
int hhp_plugin_munmap(pid_t pid,intptr_t address,size_t bytes){
  const uint64_t args[6]={(uint64_t)address,bytes,0,0,0,0};
  return (int)hhp_plugin_syscall6(pid,SYS_munmap,args);
}
