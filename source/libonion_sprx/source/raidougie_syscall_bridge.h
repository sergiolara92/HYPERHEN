// SPDX-License-Identifier: GPL-3.0-or-later
// Exact Rai indirect-syscall ABI adapter. No syscall instruction is emitted.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HHP_RAI_SYSCALL_BRIDGE_BYTES 36u

// Caller must first verify gate points to executable libkernel bytes 0f 05 c3
// in this exact live game generation. The address check here is not that proof.
// The patched PRX calls this bridge: the sixth original argument is [rsp+16],
// accounting for both its original return address and the extra bridge return.
// MOV/JMP and the native RET preserve carry for the original PRX errno suffix.
static inline int hhp_raidougie_syscall_bridge(void *out,size_t capacity,uint64_t gate){
    if(!out||capacity<HHP_RAI_SYSCALL_BRIDGE_BYTES||gate<UINT64_C(0x10000)||
       gate>UINT64_C(0x800000000000)-3)return -1;
    unsigned char code[HHP_RAI_SYSCALL_BRIDGE_BYTES]={
        0x48,0x89,0xf8,                 // rax = requested syscall number
        0x48,0x89,0xf7,                 // rdi = arg1
        0x48,0x89,0xd6,                 // rsi = arg2
        0x48,0x89,0xca,                 // rdx = arg3
        0x4d,0x89,0xc2,                 // r10 = arg4
        0x4d,0x89,0xc8,                 // r8  = arg5
        0x4c,0x8b,0x4c,0x24,0x10,       // r9  = arg6
        0x49,0xbb,0,0,0,0,0,0,0,0,      // r11 = verified libkernel gate
        0x41,0xff,0xe3                  // tail jump; native RET resumes PRX
    };
    memcpy(code+25,&gate,sizeof gate);
    memcpy(out,code,sizeof code);return 0;
}
