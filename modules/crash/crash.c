/*
 * crash -- bring the whole machine down, on purpose.
 *
 * Writes to address zero. With no memory protection between programs yet
 * (design section 82, step 4 of the plan) that is not this process's
 * fault to be stopped for: it is a panic, and the board resets. Which is
 * the test: put it in /f0/startup and the board should be in safe mode at
 * the third boot, with the crash in `boots` and `log -p`.
 *
 *   crash          write to address 0: a panic
 *   crash hang     never give the CPU back: the watchdog's to notice, in
 *                  two minutes, and a reset `boots` says was the task
 *                  watchdog's
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL) return 2;
    if (env->arg && m_eq(env->arg, "hang")) {
        m_say(env, RV9_STDOUT, "crash: holding the CPU; the watchdog has two minutes\n");
        env->sleep_ms(200);
        for (;;) { __asm__ volatile ("" ::: "memory"); }
    }
    m_say(env, RV9_STDOUT, "crash: writing to address 0\n");
    env->sleep_ms(200);                     /* let the line out */
    *(volatile uint32_t *)0 = 0xdead;
    return 0;
}
