// SPDX-License-Identifier: GPL-3.0-or-later
#include "raidougie_compat.h"

static int user_range(uint64_t at, uint64_t size) {
    return at>=UINT64_C(0x10000) && at<UINT64_C(0x800000000000) &&
           size<=UINT64_C(0x800000000000)-at;
}
static uint64_t stack_word(uint64_t at) {
    const volatile unsigned char *p=(const volatile unsigned char *)(uintptr_t)at;
    uint64_t value=0;
    for(unsigned i=0;i<8;++i) value|=(uint64_t)p[i]<<(i*8);
    return value;
}

__attribute__((section(".text.entry"),noinline))
int HHP_RAI_SYSV hhp_raidougie_bridge(const HhpRaiControl *control, uint64_t command,
    void *argument, uint64_t caller_frame, uint64_t caller_stack, uint64_t module_base) {
    // Command0 is merely logged by the exact module; do not advertise a fake
    // complete GoldHEN SDK. Memory-write command5 and all others stay absent.
    if(command!=4 || !control || !argument ||
       !user_range((uintptr_t)control,sizeof *control) ||
       !user_range((uintptr_t)argument,sizeof(HhpRaiInfo))) return -1;
    // Canonical addresses alone do not imply writable memory. Admit only the
    // exact original module's verified main -> cmd4 -> common-wrapper chain.
    // RSP is captured by our stub while a live return address is on that stack.
    // Check that return before following any saved frame. Exported wrappers
    // called by other modules or different code paths fail without an output
    // write; this is deliberately not a general syscall/EFAULT implementation.
    if(!user_range(module_base,0x80c9b) ||
       !user_range(caller_stack,8) || (caller_stack&7) ||
       !user_range(caller_frame,16) || (caller_frame&15) ||
       caller_frame<caller_stack || caller_frame-caller_stack!=0x18 ||
       stack_word(caller_stack)!=module_base+0x80c9a) return -1;
    const uint64_t main_frame=stack_word(caller_frame);
    if(main_frame<caller_frame || main_frame-caller_frame!=0x520 ||
       stack_word(caller_frame+8)!=module_base+0x7f491 ||
       stack_word(caller_frame-8)!=(uintptr_t)argument ||
       (uintptr_t)argument!=main_frame-0xe8 ||
       (uintptr_t)argument<main_frame-0x510 ||
       sizeof(HhpRaiInfo)>main_frame-(uintptr_t)argument) return -1;
    const uint64_t source=(uintptr_t)control, destination=(uintptr_t)argument;
    if(destination<source+sizeof *control && source<destination+sizeof(HhpRaiInfo)) return -1;
    if(control->magic!=HHP_RAI_MAGIC || !control->cookie ||
       control->version!=HHP_RAI_VERSION || control->bytes!=sizeof *control ||
       control->info.pid<=1 || !user_range(control->info.base,1)) return -1;
    // Snapshot is read-only in the target. The admitted active frame owns the
    // entire destination buffer. Volatile byte copies avoid implicit libc calls, unaligned
    // packed-field accesses, and writes outside the exact202-byte wire object.
    const volatile unsigned char *in=(const volatile unsigned char *)&control->info;
    volatile unsigned char *out=(volatile unsigned char *)argument;
    for(size_t i=0;i<sizeof(HhpRaiInfo);++i) out[i]=in[i];
    return 0;
}
