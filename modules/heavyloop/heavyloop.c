/*
 * heavyloop -- a slow loop that does a lot of work each period.
 *
 *   rt heavyloop
 *
 * Every 100 ms, 20 ms of computation. Its deadline is its period and it
 * meets it easily; what matters is that for a fifth of the time it is
 * busy, and anything sharing its priority waits for it. Paired with
 * `fastloop` it is the workload one priority cannot serve and two can.
 */
#include "modlib.h"

#define RUNS     20         /* two seconds */
#define WORK_US  20000

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;
    if (env->rt_declare == NULL || env->rt_wait == NULL) {
        m_say(env, RV9_STDOUT, "heavyloop: run it with: rt heavyloop\n");
        return -2;
    }

    if (env->rt_declare(0) < 0) return -4;

    for (uint32_t n = 0; n < RUNS; n++) {
        uint64_t t0 = env->time_us();
        while (env->time_us() - t0 < WORK_US) { }
        if (env->rt_wait() < 0) break;
    }
    return 0;
}
