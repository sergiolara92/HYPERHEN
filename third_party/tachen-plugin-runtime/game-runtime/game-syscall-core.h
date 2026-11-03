/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <onion/pt.h>

enum { HHP_TRACE_PENDING=0, HHP_TRACE_STOPPED=1, HHP_TRACE_EXITED=2, HHP_TRACE_POLL_ERROR=3 };
typedef struct { int kind,signal,error; } HhpPluginTraceEvent;
typedef struct {
  void *context;
  intptr_t (*resolve)(void*,pid_t);
  int (*read)(void*,pid_t,intptr_t,void*,size_t);
  int (*getregs)(void*,pid_t,struct reg*);
  int (*setregs)(void*,pid_t,const struct reg*);
  int (*step)(void*,pid_t);
  HhpPluginTraceEvent (*poll)(void*,pid_t);
  int (*stop)(void*,pid_t);
  int (*now)(void*,uint64_t*);
  void (*sleep)(void*,unsigned);
} HhpPluginTraceOps;
enum {
  HHP_SYSCALL_OK=0, HHP_SYSCALL_GATE=1, HHP_SYSCALL_REGS=2,
  HHP_SYSCALL_STEP=3, HHP_SYSCALL_WAIT=4, HHP_SYSCALL_TIMEOUT=5,
  HHP_SYSCALL_RESTORE=6, HHP_SYSCALL_REMOTE=7
};
typedef struct { long value; int status,uncertain,restored,error; } HhpPluginSyscallResult;
/* Caller owns an already stopped process and its ptrace credentials. */
HhpPluginSyscallResult hhp_plugin_syscall_transaction(const HhpPluginTraceOps*,pid_t,int,const uint64_t[6]);
