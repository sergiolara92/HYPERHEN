// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TACHEN_PROCESS_INFO_H
#define TACHEN_PROCESS_INFO_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <sys/types.h>
#include <sys/user.h>

// Read-only observations from the same calls used by hhp_process_info.
// requested is the size-probe result; returned is the record-read result.
// status preserves a failing sysctl return; validation failures use -1.
// error is meaningful on failure; successful reads report zero here without
// changing the legacy function's errno behavior.
typedef struct HhpProcessInfoDiagnostic {
 const char *stage;
 int status, error, declared;
 size_t requested, returned;
 int observed_pid;
} HhpProcessInfoDiagnostic;

int hhp_process_info_ex(pid_t pid,struct kinfo_proc* out,HhpProcessInfoDiagnostic* diagnostic);
int hhp_process_info(pid_t pid,struct kinfo_proc* out);
#ifdef __cplusplus
}
#endif
#endif
