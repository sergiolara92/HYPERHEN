// SPDX-License-Identifier: GPL-3.0-or-later
// Process-info wire layout follows the supplied TacHen/GoldHEN SDK v1 contract.
// This adapter serves only the separately verified RaiDougie binary identity.
#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(__x86_64__) && defined(_WIN32)
#define HHP_RAI_SYSV __attribute__((sysv_abi))
#else
#define HHP_RAI_SYSV
#endif
#define HHP_RAIDOUGIE_SHA "15dde8a561a6d50f7387815e16c78df678b06aa7bdb53b8e0d8fa5c44e94b26c"
#define HHP_RAIDOUGIE_RAW_SHA "e7b25eb1c6245f1ef3f97a301c23fa53baa33ce46a09c9834208118dd57b5358"
#define HHP_RAI_MAGIC UINT64_C(0x4859504552524131)
#define HHP_RAI_VERSION 1u
#define HHP_RAI_WRAPPER_OFFSET 0x819e0u
#define HHP_RAI_RAW_WRAPPER_OFFSET 0x84bf0u
#define HHP_RAI_WRAPPER_VADDR 0x80bf0u
#define HHP_RAI_WRAPPER_BYTES 42u
#define HHP_RAI_SYSCALL_VADDR 0x80da0u
#define HHP_RAI_SYSCALL_OFFSET 0x81b90u
#define HHP_RAI_RAW_SYSCALL_OFFSET 0x84da0u
#define HHP_RAI_SYSCALL_BYTES 43u
#define HHP_RAI_SYSCALL_PREFIX_BYTES 12u
#define HHP_RAI_FILE_BYTES 1117920u
#define HHP_RAI_RAW_FILE_BYTES 1189544u

typedef struct __attribute__((packed)) {
    int32_t pid;
    char name[40], path[64], title[16], content[64], version[6];
    uint64_t base;
} HhpRaiInfo;

// An immutable snapshot, owned by one game process generation until it exits.
// Host publishes and verifies it before the module can enter the private gate.
typedef struct {
    uint64_t magic, cookie;
    uint32_t version, bytes;
    HhpRaiInfo info;
} HhpRaiControl;

#if defined(__cplusplus)
static_assert(sizeof(HhpRaiInfo)==202 && offsetof(HhpRaiInfo,base)==194);
static_assert(sizeof(HhpRaiControl)==232 && offsetof(HhpRaiControl,info)==24);
extern "C" {
#else
_Static_assert(sizeof(HhpRaiInfo)==202 && offsetof(HhpRaiInfo,base)==194,"Process-info ABI");
_Static_assert(sizeof(HhpRaiControl)==232 && offsetof(HhpRaiControl,info)==24,"Private snapshot ABI");
#endif

int hhp_raidougie_supported(const char *sha256);
// 0 means exact SELF or equivalent extracted ELF admitted; -1 means no adaptation. No input writes.
int hhp_raidougie_admit(const void *original, size_t size);
// Host must supply truthful current-game metadata. Failure leaves out intact.
int hhp_raidougie_control_init(HhpRaiControl *out, uint64_t cookie, const HhpRaiInfo *info);
// Only a fresh, verified per-request backing copy may be modified. On failure
// no byte changes; on success the process-info wrapper and the generic syscall
// prefix are replaced. Its original carry/errno/return suffix stays intact.
// syscall_bridge_entry points to a separately verified native-API bridge.
int hhp_raidougie_patch(void *staged, size_t size, uint64_t bridge_entry,
    uint64_t control_address, uint64_t syscall_bridge_entry);
// Freestanding private userland service: command4 only; other commands fail.
// Extra arguments are captured by the private42-byte stub, not supplied by the
// module: its caller RBP/RSP and the current PRX slide derived from stub RIP.
int HHP_RAI_SYSV hhp_raidougie_bridge(const HhpRaiControl *control, uint64_t command,
    void *argument, uint64_t caller_frame, uint64_t caller_stack, uint64_t module_base);

#if defined(__cplusplus)
}
#endif
