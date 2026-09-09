/*
 * worker -- a deliberately CPU-bound module, used to demonstrate scheduling.
 *
 * It never blocks, so under a plain priority scheduler a low-priority
 * instance gets no CPU at all while a high-priority one is running. That is
 * the starvation the process manager's aging policy exists to prevent, and
 * this module is how we prove it happens and then prove it stops happening.
 *
 * Work is measured in units rather than seconds so the two instances can be
 * compared directly. The run is bounded by wall clock so a starved instance
 * still terminates rather than hanging the demo.
 */
#include "rv9/module.h"

#define WORK_MS       1200u
#define BURN_ROUNDS   20000u

typedef struct {
    uint32_t units;
    uint32_t stopped_early;
} worker_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)                        return -1;
    if (env->abi_version < RV9_MODULE_ABI)  return -2;
    if (env->time_ms == NULL)               return -3;
    if (env->signals_take == NULL)          return -4;

    worker_statics_t *st = (worker_statics_t *)env->statics;
    if (st == NULL)                         return -5;
    if (env->statics_size < sizeof(*st))    return -6;

    const uint64_t deadline = env->time_ms() + WORK_MS;
    uint32_t units = 0;

    while (env->time_ms() < deadline) {
        if (env->signals_take() & RV9_SIG_STOP) {
            st->stopped_early = 1;
            break;
        }

        /* Burn a measurable, repeatable amount of CPU. volatile so the
           optimiser cannot discover that none of this matters. */
        volatile uint32_t acc = 0;
        for (uint32_t i = 0; i < BURN_ROUNDS; i++) {
            acc += i;
        }

        units++;

        /* Publish progress every iteration rather than only at the end.
           A starved process reports zero here, which is the whole point --
           final counts cannot show starvation, because a starved process
           simply starts its clock late and then runs unimpeded. */
        st->units = units;
    }

    st->units = units;
    return (int)units;
}
