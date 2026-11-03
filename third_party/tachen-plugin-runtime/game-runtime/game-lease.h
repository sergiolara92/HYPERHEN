/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller owns the existing ptrace credential window. Ownership survives a
 * failed stop wait, so the caller must detach even after an error. */
int hhp_plugin_attach_owned(pid_t pid, int *owned);
int hhp_plugin_getlwps(pid_t pid, int *ids, size_t capacity);
#ifdef __cplusplus
}
#endif
