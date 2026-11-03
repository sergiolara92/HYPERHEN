/* SPDX-License-Identifier: GPL-3.0-or-later
 * Scoped adaptation of TacHen platform/loader/pt.c, John Tornblom's ptrace
 * interface. Does not replace HyperHEN's shared ShellUI transport. */
#include "game-lease.h"
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int hhp_plugin_attach_owned(pid_t pid, int *owned) {
  if(!owned){errno=EINVAL;return -1;}
  *owned=0;
  if(syscall(SYS_ptrace,PT_ATTACH,pid,0,0)<0)return -1;
  *owned=1;
  struct timespec start,now;
  if(clock_gettime(CLOCK_MONOTONIC,&start))return -1;
  for(;;){
    int status=0;pid_t got=waitpid(pid,&status,WUNTRACED|WNOHANG);
    if(got==pid){if(WIFSTOPPED(status))return 0;errno=ESRCH;return -1;}
    if(got<0&&errno!=EINTR)return -1;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return -1;
    if((now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000>=1000){errno=ETIMEDOUT;return -1;}
    usleep(1000);
  }
}
int hhp_plugin_getlwps(pid_t pid,int *ids,size_t capacity){
  long count=syscall(SYS_ptrace,PT_GETNUMLWPS,pid,0,0);
  if(count<=0||(size_t)count>capacity||count>2048)return -1;
  long got=syscall(SYS_ptrace,PT_GETLWPLIST,pid,ids,count);
  return got==count?(int)got:-1;
}
