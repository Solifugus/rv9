/*
 * smash -- run off the bottom of the stack on purpose.
 *
 * A detector nobody has seen fire is a claim, not a feature. This is how
 * the guard is exercised on the real board: it descends until it is
 * standing on the guard, and is killed for it. The shell should print
 * "smash: ran off its stack and was stopped", `procs` should show STACK,
 * and nothing should be left behind.
 *
 * The descent is measured rather than guessed, which is the whole point.
 * A recursion that simply runs away writes hundreds of bytes into whoever
 * the allocator put underneath this stack, so a test of the detector
 * becomes a test of how much corruption the system survives. Instead this
 * asks how much of its own stack is left after every frame and stops the
 * moment the answer is "almost none" -- by which time the guard has been
 * written and nothing below it has.
 */
#include "modlib.h"

/*
 * Stop once fewer than this many bytes remain unwritten.
 *
 * The guard is the lowest four words, so any answer below sixteen means it
 * has already been written -- which is what we came for. Eight leaves a
 * word of slack for the measurement being one frame coarse.
 */
#define FLOOR_BYTES 8

typedef struct { rv9_sys_stack_t s[8]; } smash_t;

/*
 * Bytes of our own stack never yet touched.
 *
 * Answered from the deepest point reached so far, which includes the call
 * into the process manager that answers it. So it is a lower bound on what
 * is left -- the safe direction for it to be wrong in, and the reason the
 * descent can stop without ever taking a blind step.
 */
static int32_t spare(const rv9_mod_env_t *env, smash_t *st, uint32_t *size)
{
    int n = env->sysinfo(RV9_SYS_STACK, st->s, sizeof(st->s));
    for (int i = 0; i < n; i++) {
        if (st->s[i].pid == (uint16_t)env->pid) {
            if (size) *size = st->s[i].stack_size;
            return (int32_t)st->s[i].stack_unused;
        }
    }
    return -1;
}

/*
 * One frame at a time.
 *
 * The answer does not fall on every frame: it is a high-water mark, so it
 * sits still until the recursion is deeper than whatever set it -- module
 * entry and the first write, a few hundred bytes down. It falls steadily
 * after that. `max_depth` is only a backstop against a measurement that
 * never falls at all; on a kernel that reports honestly it is never
 * reached, because `left` gets there first.
 */
static int descend(const rv9_mod_env_t *env, smash_t *st, int depth,
                   int max_depth)
{
    /* Read back as well as written: a frame the compiler can prove nobody
       looks at is a frame it is entitled to delete. */
    volatile char frame[16];
    frame[0]  = (char)depth;
    frame[15] = (char)depth;
    if (frame[0] != frame[15]) return -3;

    int32_t left = spare(env, st, NULL);
    if (left < 0)            return -1;     /* cannot see ourselves */
    if (left <= FLOOR_BYTES) return depth;  /* standing on the guard */
    if (depth >= max_depth)  return -2;     /* it is not falling */

    /*
     * The recursion must not be a tail call, or there is no recursion.
     *
     * Written as `return descend(...)` this compiles at -Os to a jump back
     * to the top with the frame reused -- sixty-four levels deep and not
     * one byte of stack consumed, which is exactly what the first run
     * reported and what "the measurement never fell" was telling us.
     * Reading the frame afterwards forces it to outlive the call.
     */
    int deepest = descend(env, st, depth + 1, max_depth);
    if (frame[15] != (char)depth) return -3;
    return deepest;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    smash_t *st = (smash_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    /*
     * Refuse to run where the answer cannot be trusted.
     *
     * The host backend keeps a high-water mark but not a size, and reports
     * the size as zero rather than inventing one. Descending on a
     * measurement that does not track this stack is precisely the runaway
     * recursion this module exists to avoid.
     */
    uint32_t size = 0;
    if (spare(env, st, &size) < 0 || size == 0) {
        m_say(env, RV9_STDOUT, "stack use is not measurable on this kernel\n");
        return -4;
    }

    /*
     * How expensive the question is, which is most of the stack.
     *
     * Asking costs about 600 bytes of a 1024-byte stack: the call walks the
     * process table and fills a record per live process. So the descent is
     * not really the module's frames arriving at the floor -- it is the
     * *measurement* arriving there, taken from a frame 600 bytes higher.
     * Worth printing, because it is why the last step is a plunge rather
     * than a step, and why the guard now covers the pad as well (§50).
     */
    int32_t left0 = spare(env, st, NULL);
    m_say(env, RV9_STDOUT, "stack ");
    m_num(env, RV9_STDOUT, (int)size);
    m_say(env, RV9_STDOUT, ", unwritten at entry ");
    m_num(env, RV9_STDOUT, (int)left0);
    m_say(env, RV9_STDOUT, " (asking costs ");
    m_num(env, RV9_STDOUT, (int)size - left0);
    m_say(env, RV9_STDOUT, ")\ndescending...\n");

    /* One frame is at least sixteen bytes, so the whole stack cannot hold
       more than this many of them. */
    int depth = descend(env, st, 1, (int)(size / 16));
    if (depth == -1) { m_say(env, RV9_STDOUT, "cannot see my own stack\n");
                       return -3; }
    if (depth == -2) { m_say(env, RV9_STDOUT, "the measurement never fell\n");
                       return -5; }

    /*
     * Back up, with the guard written.
     *
     * Reaching this line is not a failure and was wrongly reported as one.
     * The guard is read when a thread is switched away from, and on a busy
     * board the descent itself blocks somewhere and is caught before it
     * ever returns -- which is what happens most of the time and why this
     * line was rarely seen. On a quiet board nothing blocks, the recursion
     * unwinds, and the check falls to the one made when the module returns.
     *
     * Either way the process ends with RV9_FAULT_STACK and the shell says
     * so; the status below is discarded. So this reports where it got to,
     * and leaves the verdict to whoever is entitled to give it.
     */
    m_say(env, RV9_STDOUT, "back up with the guard written, at depth ");
    m_num(env, RV9_STDOUT, depth);
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
