/* SPDX-License-Identifier: GPL-3.0-or-later
 * Private filesystem ABI adapters for the HyperHEN plugin loader.
 * The SDK's raw *at/fstatfs stubs return a positive errno with carry set.
 * Keep these adapters local: successful descriptors can also be positive.
 */
#ifndef HHP_PLUGIN_FILESYSTEM_H
#define HHP_PLUGIN_FILESYSTEM_H

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/syscall.h>

#if !defined(__x86_64__)
#error "The PS5 plugin filesystem ABI requires x86-64"
#endif

struct statfs;

static inline long hhp_fs_syscall6(long number, long arg1, long arg2,
                                  long arg3, long arg4, long arg5, long arg6) {
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = arg6;
    long result = number;
    unsigned char failed;
    __asm__ volatile("syscall\n\tsetc %1"
                     : "+a"(result), "=qm"(failed)
                     : "D"(arg1), "S"(arg2), "d"(arg3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");
    if (failed) {
        errno = (int)result;
        return -1;
    }
    return result;
}

static inline int hhp_fs_mkdirat(int directory, const char *path, mode_t mode) {
    return (int)hhp_fs_syscall6(SYS_mkdirat, (long)directory,
        (long)(intptr_t)path, (long)mode, 0, 0, 0);
}

/* Mode is explicit even without O_CREAT, avoiding variadic promotions. */
static inline int hhp_fs_openat(int directory, const char *path, int flags,
                                mode_t mode) {
    return (int)hhp_fs_syscall6(SYS_openat, (long)directory,
        (long)(intptr_t)path, (long)flags, (long)mode, 0, 0);
}

static inline int hhp_fs_fstatfs(int descriptor, struct statfs *output) {
    return (int)hhp_fs_syscall6(SYS_fstatfs, (long)descriptor,
        (long)(intptr_t)output, 0, 0, 0, 0);
}

static inline int hhp_fs_renameat(int from_directory, const char *from,
                                  int to_directory, const char *to) {
    return (int)hhp_fs_syscall6(SYS_renameat, (long)from_directory,
        (long)(intptr_t)from, (long)to_directory, (long)(intptr_t)to, 0, 0);
}

static inline int hhp_fs_unlinkat(int directory, const char *path, int flags) {
    return (int)hhp_fs_syscall6(SYS_unlinkat, (long)directory,
        (long)(intptr_t)path, (long)flags, 0, 0, 0);
}

#endif
