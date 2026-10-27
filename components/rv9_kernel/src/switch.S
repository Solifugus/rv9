/*
 * RV-9 context switch, RV32I.
 *
 * The whole kernel balances on this function. It saves the callee-saved
 * registers of the running thread onto its own stack, records that stack
 * pointer, loads another thread's, and returns -- into the other thread.
 *
 * Only callee-saved registers are here. Caller-saved registers are, by the
 * ABI's own definition, already dead across a function call, and this is a
 * function call. That is not a shortcut; it is the calling convention doing
 * half the work for us.
 *
 * On a chip with an FPU the calling convention says more, and this file
 * said nothing until 2026-10-26: under the hard-float ABI fs0-fs11 are
 * callee-saved too, and fcsr -- the rounding mode and the exception flags
 * -- belongs to the thread. The P4 is built that way and the compiler keeps
 * floats in those registers across any call, a switch included. Without
 * them two threads doing arithmetic shared one set of registers: in the
 * KAL self-test that now checks this, one thread got the other's values
 * back after every one of its fifty switches.
 * The C3 and C5 have no FPU, and their frame is unchanged.
 *
 * An fsw here with the FPU switched off is not a fault. ESP-IDF turns the
 * FPU off for a FreeRTOS task it has just switched in and enables it on
 * the first FP instruction, putting the task's own registers back first --
 * so this saves the right values whether or not anything has touched the
 * FPU since. It also means the task RV-9 runs in owns the FPU from its
 * first switch on, which costs IDF one save of it when another task
 * wants it. Saving only when the FPU looks idle would be wrong: idle can
 * mean "parked in IDF's save area", still holding this thread's values.
 *
 *   void rv9_ctx_switch(uint32_t **save_sp, uint32_t *load_sp);
 *
 * a0 = where to store the outgoing stack pointer
 * a1 = the incoming stack pointer
 */
    .section .text
    .globl rv9_ctx_switch
    .type rv9_ctx_switch, @function
    .align 2

#if defined(__riscv_flen)
#if __riscv_flen != 32
#error "the FP frame below is single precision; widen it for D"
#endif
#define CTX_WORDS 28          /* ra, s0-s11, fcsr, fs0-fs11, two of padding */
#else
#define CTX_WORDS 14          /* ra, s0-s11, and one word of padding */
#endif
#define CTX_BYTES (CTX_WORDS * 4)

/* sched.c builds a new thread's first frame and mirrors CTX_WORDS. */

.macro save_fp
#if defined(__riscv_flen)
    frcsr t0
    sw    t0,  52(sp)
    fsw   fs0, 56(sp)
    fsw   fs1, 60(sp)
    fsw   fs2, 64(sp)
    fsw   fs3, 68(sp)
    fsw   fs4, 72(sp)
    fsw   fs5, 76(sp)
    fsw   fs6, 80(sp)
    fsw   fs7, 84(sp)
    fsw   fs8, 88(sp)
    fsw   fs9, 92(sp)
    fsw   fs10,96(sp)
    fsw   fs11,100(sp)
#endif
.endm

.macro load_fp
#if defined(__riscv_flen)
    flw   fs0, 56(sp)
    flw   fs1, 60(sp)
    flw   fs2, 64(sp)
    flw   fs3, 68(sp)
    flw   fs4, 72(sp)
    flw   fs5, 76(sp)
    flw   fs6, 80(sp)
    flw   fs7, 84(sp)
    flw   fs8, 88(sp)
    flw   fs9, 92(sp)
    flw   fs10,96(sp)
    flw   fs11,100(sp)
    lw    t0,  52(sp)
    fscsr t0
#endif
.endm

rv9_ctx_switch:
    addi  sp, sp, -CTX_BYTES

    sw    ra,  0(sp)
    sw    s0,  4(sp)
    sw    s1,  8(sp)
    sw    s2, 12(sp)
    sw    s3, 16(sp)
    sw    s4, 20(sp)
    sw    s5, 24(sp)
    sw    s6, 28(sp)
    sw    s7, 32(sp)
    sw    s8, 36(sp)
    sw    s9, 40(sp)
    sw    s10,44(sp)
    sw    s11,48(sp)
    save_fp

    sw    sp, 0(a0)           /* outgoing thread remembers where it stopped */
    mv    sp, a1              /* and we continue on the incoming stack */

    lw    ra,  0(sp)
    lw    s0,  4(sp)
    lw    s1,  8(sp)
    lw    s2, 12(sp)
    lw    s3, 16(sp)
    lw    s4, 20(sp)
    lw    s5, 24(sp)
    lw    s6, 28(sp)
    lw    s7, 32(sp)
    lw    s8, 36(sp)
    lw    s9, 40(sp)
    lw    s10,44(sp)
    lw    s11,48(sp)
    load_fp

    addi  sp, sp, CTX_BYTES
    ret                       /* returns into whoever this stack belongs to */

    .size rv9_ctx_switch, . - rv9_ctx_switch

/*
 * Leave the current thread without saving it.
 *
 *   void rv9_ctx_load(uint32_t *load_sp);   -- does not return
 *
 * The half of a switch that abandons the outgoing context. It exists for
 * the one case where saving would be actively harmful: a thread that ran
 * off the bottom of its own stack. Pushing another frame onto that
 * stack writes further into whatever the allocator put underneath it, so
 * a switch that saves turns one victim's overflow into two victims. The
 * thread is dead and will never be resumed, so its registers are worth
 * nothing -- drop them.
 */
    .globl rv9_ctx_load
    .type rv9_ctx_load, @function
    .align 2

rv9_ctx_load:
    mv    sp, a0

    lw    ra,  0(sp)
    lw    s0,  4(sp)
    lw    s1,  8(sp)
    lw    s2, 12(sp)
    lw    s3, 16(sp)
    lw    s4, 20(sp)
    lw    s5, 24(sp)
    lw    s6, 28(sp)
    lw    s7, 32(sp)
    lw    s8, 36(sp)
    lw    s9, 40(sp)
    lw    s10,44(sp)
    lw    s11,48(sp)
    load_fp

    addi  sp, sp, CTX_BYTES
    ret

    .size rv9_ctx_load, . - rv9_ctx_load

/*
 * Where a new thread begins.
 *
 * A fresh stack is built so that the first switch into it "returns" here,
 * with s0 holding the entry point and s1 the argument. Those are
 * callee-saved, so the switch above restores them for us -- a new thread
 * and a resumed one take exactly the same path in.
 */
    .globl rv9_thread_trampoline
    .type rv9_thread_trampoline, @function
    .align 2

rv9_thread_trampoline:
    mv    a0, s1              /* argument */
    jalr  s0                  /* entry point */
    j     rv9_thread_exited   /* falling off the end is still an exit */

    .size rv9_thread_trampoline, . - rv9_thread_trampoline
