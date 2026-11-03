/* SPDX-License-Identifier: GPL-3.0-or-later
 * Scoped adaptation of TacHen beta.3 platform/loader/pt.c (John Tornblom and
 * contributors): verified syscall instruction, one step, finite waits.
 */
#include "game-syscall-core.h"
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <onion/log.h>

static int wait_stop(const HhpPluginTraceOps *o,pid_t pid,unsigned timeout,HhpPluginTraceEvent *event){
  uint64_t start=0,now=0;
  if(o->now(o->context,&start))return -1;
  for(unsigned polls=0;polls<=timeout+1;++polls){
    *event=o->poll(o->context,pid);
    if(event->kind!=HHP_TRACE_PENDING)return event->kind==HHP_TRACE_STOPPED?0:-1;
    if(o->now(o->context,&now)||now<start)return -1;
    if(now-start>=timeout)return 1;
    o->sleep(o->context,1);
  }
  return 1;
}
HhpPluginSyscallResult hhp_plugin_syscall_transaction(const HhpPluginTraceOps *o,pid_t pid,int sysno,const uint64_t args[6]){
  HhpPluginSyscallResult out={.value=-1,.status=HHP_SYSCALL_GATE,.error=EIO};
  if(!o||!args||!o->resolve||!o->read||!o->getregs||!o->setregs||!o->step||
     !o->poll||!o->stop||!o->now||!o->sleep||pid<=1)return out;
  intptr_t function=o->resolve(o->context,pid);
  if(function<0x10000||(uint64_t)function>UINT64_C(0x7fffffffffff)-12){
    LOG_INFO("[plugins] gate-resolution-failed pid=%d errno=%d",pid,errno);return out;
  }
  intptr_t gate=function+0xa;unsigned char opcode[2]={0};
  if(o->read(o->context,pid,gate,opcode,sizeof opcode)){
    LOG_INFO("[plugins] gate-recheck-read-failed pid=%d errno=%d",pid,errno);return out;
  }
  if(opcode[0]!=0x0f||opcode[1]!=0x05){
    LOG_INFO("[plugins] gate-recheck-opcode-mismatch pid=%d bytes=%02x%02x",pid,opcode[0],opcode[1]);return out;
  }
  struct reg saved,jump;
  out.status=HHP_SYSCALL_REGS;
  if(o->getregs(o->context,pid,&saved))return out;
  memcpy(&jump,&saved,sizeof jump);jump.r_rip=gate;jump.r_rax=sysno;
  jump.r_rdi=args[0];jump.r_rsi=args[1];jump.r_rdx=args[2];
  jump.r_r10=args[3];jump.r_r8=args[4];jump.r_r9=args[5];
  /* RSP and every non-syscall register retain the interrupted values. */
  if(o->setregs(o->context,pid,&jump)){
    out.restored=!o->setregs(o->context,pid,&saved);
    out.uncertain=!out.restored;return out;
  }
  int stopped=1;
  if(o->step(o->context,pid))out.status=HHP_SYSCALL_STEP;
  else {
    stopped=0;HhpPluginTraceEvent event={0};int waited=wait_stop(o,pid,1000,&event);
    if(!waited){
      stopped=1;
      if(event.signal!=SIGTRAP){out.status=HHP_SYSCALL_WAIT;out.uncertain=1;}
      else if(o->getregs(o->context,pid,&jump)){out.status=HHP_SYSCALL_REGS;out.uncertain=1;}
      else if(jump.r_rip!=(uint64_t)gate+2){out.status=HHP_SYSCALL_STEP;out.uncertain=1;}
      else if(jump.r_rflags&1){
        out.status=HHP_SYSCALL_REMOTE;
        out.error=jump.r_rax>0&&jump.r_rax<4096?(int)jump.r_rax:EIO;
      } else {out.status=HHP_SYSCALL_OK;out.value=(long)jump.r_rax;out.error=0;}
    } else {
      out.status=waited==1?HHP_SYSCALL_TIMEOUT:HHP_SYSCALL_WAIT;
      out.error=waited==1?ETIMEDOUT:event.error?event.error:EIO;
      /* The syscall may already have run. Its effects cannot be rolled back
       * from a late stop with an unknown return value. Retain that uncertainty. */
      out.uncertain=1;
      if(event.kind!=HHP_TRACE_EXITED&&!o->stop(o->context,pid)){
        HhpPluginTraceEvent recovery={0};
        stopped=!wait_stop(o,pid,1000,&recovery)&&recovery.kind==HHP_TRACE_STOPPED;
      }
    }
  }
  if(stopped){
    out.restored=!o->setregs(o->context,pid,&saved);
    if(!out.restored){out.status=HHP_SYSCALL_RESTORE;out.uncertain=1;out.value=-1;out.error=EIO;}
  } else {out.status=HHP_SYSCALL_RESTORE;out.uncertain=1;out.value=-1;}
  if(out.status!=HHP_SYSCALL_OK)out.value=-1;
  return out;
}
