/*
 * fastloop -- a loop with a lot of work and a tight deadline, for priority
 * to protect.
 *
 *   rt fastloop
 *
 * Every 5 ms, 2 ms of work, due within 3 ms of release, and a miss is
 * fatal. On its own it never misses.
 *
 * Beside `heavyloop` at the same priority it does. The heavy loop's work
 * is 15 ms, and whenever its release lands while this loop is working --
 * two times in five -- the scheduler lets the heavy loop in: an equal
 * priority is not an exclusion. This loop then finishes 15 ms late and is
 * stopped. Placed by deadline, the heavy loop cannot run ahead of it at
 * all.
 *
 * An earlier version did 100 us of work against a 500 us deadline and
 * relied on waiting for a scheduler tick to miss. This host wakes a task of
 * equal priority immediately, so the miss came and went between boots --
 * a control experiment that only sometimes controls is not one.
 */
#include "modlib.h"

#define RUNS     400        /* two seconds */
#define WORK_US  2000

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;
    if (env->rt_declare == NULL || env->rt_wait == NULL) {
        m_say(env, RV9_STDOUT, "fastloop: run it with: rt fastloop\n");
        return -2;
    }

    if (env->rt_declare(0) < 0) return -4;

    for (uint32_t n = 0; n < RUNS; n++) {
        uint64_t t0 = env->time_us();
        while (env->time_us() - t0 < WORK_US) { }
        if (env->rt_wait() < 0) break;
    }

    rv9_rt_report_t r;
    if (env->rt_stats(&r) == 0) {
        m_say(env, RV9_STDOUT, "fastloop: ");
        m_num(env, RV9_STDOUT, (int32_t)r.activations);
        m_say(env, RV9_STDOUT, " on time, worst jitter ");
        m_num(env, RV9_STDOUT, (int32_t)r.max_jitter_us);
        m_say(env, RV9_STDOUT, " us\n");
    }
    return 0;
}
