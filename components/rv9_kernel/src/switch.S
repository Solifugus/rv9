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
 *   void rv9_ctx_switch(uint32_t **save_sp, uint32_t *load_sp);
 *
 * a0 = where to store the outgoing stack pointer
 * a1 = the incoming stack pointer
 */
    .section .text
    .globl rv9_ctx_switch
    .type rv9_ctx_switch, @function
    .align 2

#define CTX_WORDS 14          /* ra, s0-s11, and one word of padding */
#define CTX_BYTES (CTX_WORDS * 4)

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

    addi  sp, sp, CTX_BYTES
    ret                       /* returns into whoever this stack belongs to */

    .size rv9_ctx_switch, . - rv9_ctx_switch

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
