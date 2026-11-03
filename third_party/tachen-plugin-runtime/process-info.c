// SPDX-License-Identifier: GPL-3.0-or-later
#include "process-info.h"
#include <sys/sysctl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int hhp_process_info_ex(pid_t pid,struct kinfo_proc* out,HhpProcessInfoDiagnostic* diagnostic){
 HhpProcessInfoDiagnostic unused={0};
 HhpProcessInfoDiagnostic* detail=diagnostic?diagnostic:&unused;
 memset(detail,0,sizeof *detail);detail->stage="invalid-argument";
 if(!out||pid<=0){detail->status=-1;detail->error=EINVAL;errno=EINVAL;return -1;}
 memset(out,0,sizeof *out);
 // PS5 13.60 returns 1096 bytes while SDK 0.43 describes a 1088-byte
 // prefix. Ask the kernel for its complete record before copying that prefix.
 const size_t start_end=offsetof(struct kinfo_proc,ki_start)+sizeof out->ki_start;
 const size_t name_end=offsetof(struct kinfo_proc,ki_comm)+sizeof out->ki_comm;
 const size_t minimum=start_end>name_end?start_end:name_end;
 int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,pid};size_t needed=0;
 detail->stage="size-probe";
 int result=sysctl(mib,4,NULL,&needed,NULL,0);
 detail->requested=needed;
 if(result){detail->status=result;detail->error=errno;return -1;}
 detail->stage="record-size";
 if(needed<minimum||needed>65536){detail->status=-1;detail->error=EINVAL;errno=EINVAL;return -1;}
 detail->stage="allocation";
 void* record=malloc(needed);if(!record){detail->status=-1;detail->error=ENOMEM;errno=ENOMEM;return -1;}
 detail->stage="record-read";
 size_t length=needed;result=sysctl(mib,4,record,&length,NULL,0);int error=errno;
 detail->returned=length;
 if(!result){
  detail->stage="record-layout";
  int declared=0;
  if(length>=sizeof declared&&length<=needed)memcpy(&declared,record,sizeof declared);
  detail->declared=declared;
  if(length<minimum||length>needed||declared<0||(size_t)declared!=length){result=-1;error=EINVAL;}
  else{
   struct kinfo_proc info={0};memcpy(&info,record,length<sizeof info?length:sizeof info);
   detail->observed_pid=info.ki_pid;
   if(info.ki_pid!=pid){detail->stage="pid-mismatch";result=-1;error=ESRCH;}
   else{info.ki_comm[COMMLEN]=0;*out=info;detail->stage="ok";}
  }
 }
 detail->status=result;detail->error=result?error:0;
 free(record);if(result)errno=error;return result?-1:0;
}

int hhp_process_info(pid_t pid,struct kinfo_proc* out){
 return hhp_process_info_ex(pid,out,NULL);
}
