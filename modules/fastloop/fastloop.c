/*
 * fastloop -- a fast loop with a tight deadline, for priority to protect.
 *
 *   rt fastloop
 *
 * Every 5 ms, 100 us of work, due within 500 us of release, and a miss is
 * fatal. On its own it never misses. Beside `heavyloop` -- 20 ms of work in
 * every 100 -- at the same priority, it waits for the next scheduler tick
 * whenever it is released in the middle of that work, answers late, and is
 * stopped. The miss is the scheduler's, not the loop's: which is what
 * deriving priority from deadlines is for, and what the two together show.
 */
#include "modlib.h"

#define RUNS     400        /* two seconds */
#define WORK_US  100

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
