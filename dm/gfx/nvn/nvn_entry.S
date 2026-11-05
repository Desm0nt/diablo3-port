    .arch_extension lse
// 未实现 NVN 函数的公共入口：跳板进来时 x16 = 本函数的槽地址（nvn_slot[i]）。
// 调 nvn_unimpl_c(slot, x0, x1, x2) 报红记名，返回 0。
// Common entry for unimplemented NVN functions: when the thunk comes in, x16 = this function's slot address (nvn_slot[i]).
// Calls nvn_unimpl_c(slot, x0, x1, x2) to report a failure and log the name, returns 0.
    .text
    .globl _nvn_unimpl_entry
    .p2align 2
_nvn_unimpl_entry:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x16
    bl _nvn_unimpl_c
    ldp x29, x30, [sp], #16
    ret

// 计时包装入口（DM_NVN_PROF=1 时槽表指向这里）：跳板进来时 x16 = 本函数槽地址。
// 保存 x0-x7 / q0-q7（NVN 函数无栈传参，生成器已核），取 mach_absolute_time → 调真实现（nvn_real[i]）→ 再取，
// 差值原子累加到 nvn_prof_ticks[i]（mach 时基单位）；返回值 x0/x1、q0-q3 原样带回。
// 不用 cntvct_el0：本机实测它与系统时基不成固定比例（cntfrq 报 1 GHz，同区间增量比 98.9），不能换算成时间。
// Timing wrapper entry (the slot table points here when DM_NVN_PROF=1): when the thunk comes in, x16 = this function's slot address.
// Save x0-x7 / q0-q7 (NVN functions pass no arguments on the stack, verified by the generator), take mach_absolute_time -> call the real implementation (nvn_real[i]) -> take it again,
// atomically add the difference to nvn_prof_ticks[i] (mach timebase units); return values x0/x1, q0-q3 are passed back unchanged.
// cntvct_el0 is not used: measured on this machine it is not in a fixed ratio to the system timebase (cntfrq reports 1 GHz, increment ratio over the same interval 98.9), so it cannot be converted to time.
// [DOOM] Two entries with the same body (assembler macros, so the LSE entry assembles to exactly the instructions it had):
// _nvn_prof_entry adds with the LSE stadd (ARMv8.1); _nvn_prof_entry_v80 adds with an LL/SC loop for ARMv8.0 cores without LSE
// (Cortex-A53/A57/A72/A73: stadd raises SIGILL, GitHub issue #10). hle_nvn.c picks one per run (dm_cpu_lse(), host/dm_cpu.h).
// Here x11 / x12 are free (caller-saved, the return values are already on the stack), so the fallback can be exact.
    .macro NVN_PROF_HEAD
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    adrp x9, _nvn_slot@PAGE
    add x9, x9, _nvn_slot@PAGEOFF
    sub x16, x16, x9
    lsr x16, x16, #4                 // 槽号 i | Slot index i
    str x16, [sp, #80]
    bl _mach_absolute_time
    str x0, [sp, #88]
    ldr x16, [sp, #80]
    adrp x9, _nvn_real@PAGE
    add x9, x9, _nvn_real@PAGEOFF
    ldr x17, [x9, x16, lsl #3]
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    blr x17
    stp x0, x1, [sp, #16]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    bl _mach_absolute_time
    mov x9, x0
    ldr x10, [sp, #88]
    sub x9, x9, x10
    ldr x16, [sp, #80]
    adrp x10, _nvn_prof_ticks@PAGE
    add x10, x10, _nvn_prof_ticks@PAGEOFF
    add x10, x10, x16, lsl #3
    .endm
    .macro NVN_PROF_TAIL
    ldp x0, x1, [sp, #16]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret
    .endm

    .globl _nvn_prof_entry
    .p2align 2
_nvn_prof_entry:
    NVN_PROF_HEAD
    stadd x9, [x10]
    NVN_PROF_TAIL

    .globl _nvn_prof_entry_v80
    .p2align 2
_nvn_prof_entry_v80:
    NVN_PROF_HEAD
1:  ldxr x11, [x10]
    add x11, x11, x9
    stxr w12, x11, [x10]
    cbnz w12, 1b
    NVN_PROF_TAIL

// [ETERNAL] Trace wrapper (DM_NVN_TRACE=1: every slot points here): same frame as the timing wrapper, calls
// nvn_trace_c(i, x0, x1, x2) to record the call in a ring (printed on a crash), then the real implementation (nvn_real[i]).
    .globl _nvn_trace_entry
    .p2align 2
_nvn_trace_entry:
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    adrp x9, _nvn_slot@PAGE
    add x9, x9, _nvn_slot@PAGEOFF
    sub x16, x16, x9
    lsr x16, x16, #4
    str x16, [sp, #80]
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x16
    ldr x4, [sp, #8]
    bl _nvn_trace_c
    ldr x16, [sp, #80]
    adrp x9, _nvn_real@PAGE
    add x9, x9, _nvn_real@PAGEOFF
    ldr x17, [x9, x16, lsl #3]
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    blr x17
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret

// [ETERNAL] Big-stack wrapper (every implemented slot under DM_GAME_ETERNAL): guest threads run on the stacks the game gave them
// (Bink / job threads: a few KiB), but an NVN call can submit to Vulkan and the driver needs far more (stack overflow in
// pthread_mutex_lock under vkCmdEndRenderPass from nvnSyncWait). nvn_bigstack_top(sp) returns the top of this thread's 1 MiB host
// stack, or 0 when sp is already on it; the real implementation (nvn_real_big[i]) then runs there. Arguments x0-x7 / q0-q7 are
// reloaded from the guest-stack frame (x29), return values x0 / x1 / q0-q3 come back unchanged.
    .globl _nvn_bigstack_entry
    .p2align 2
_nvn_bigstack_entry:
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    adrp x9, _nvn_slot@PAGE
    add x9, x9, _nvn_slot@PAGEOFF
    sub x16, x16, x9
    lsr x16, x16, #4
    str x16, [sp, #80]
    mov x0, sp
    bl _nvn_bigstack_top
    str x0, [x29, #88]
    ldr x16, [x29, #80]
    adrp x9, _nvn_real_big@PAGE
    add x9, x9, _nvn_real_big@PAGEOFF
    ldr x17, [x9, x16, lsl #3]
    ldr x9, [x29, #88]
    ldp x0, x1, [x29, #16]
    ldp x2, x3, [x29, #32]
    ldp x4, x5, [x29, #48]
    ldp x6, x7, [x29, #64]
    ldp q0, q1, [x29, #96]
    ldp q2, q3, [x29, #128]
    ldp q4, q5, [x29, #160]
    ldp q6, q7, [x29, #192]
    cbz x9, 1f
    mov sp, x9
    blr x17
    mov sp, x29
    b 2f
1:  blr x17
2:  ldp x29, x30, [sp]
    add sp, sp, #240
    ret
