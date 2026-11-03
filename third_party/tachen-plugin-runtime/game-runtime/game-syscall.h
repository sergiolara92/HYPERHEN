/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Begin is called by the owner of the plugin operation. Reusing the same
 * generation never clears an uncertain transport outcome. */
void hhp_plugin_transport_begin(pid_t,int64_t,int64_t);
int hhp_plugin_transport_healthy(pid_t);
/* Unknown remote effects may be held even with restored registers. Resume is
 * allowed only when the interrupted register state was confirmed restored. */
int hhp_plugin_transport_can_resume(pid_t);
/* Read-only resolution for the admitted module's native ABI adapter. Unlike
 * the single-step transport, requires a complete executable syscall;ret. */
intptr_t hhp_plugin_native_syscall_gate(pid_t);
long hhp_plugin_syscall6(pid_t,int,const uint64_t[6]);
intptr_t hhp_plugin_mmap(pid_t,intptr_t,size_t,int,int,int,off_t);
int hhp_plugin_mprotect(pid_t,intptr_t,size_t,int);
int hhp_plugin_munmap(pid_t,intptr_t,size_t);
#ifdef __cplusplus
}
#endif
