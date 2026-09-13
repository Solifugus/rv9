/*
 * deaf -- a process that is asked to stop and does not.
 *
 * It reads its signals, so it has been told, and ignores them. That is the
 * case `kill` exists for: a program that has stopped listening, whether
 * through a bug, a wait that never ends, or simply never having been
 * written to care. Asking is not enough, and something has to be.
 *
 * It holds nothing, and sleeps between looks, so it is always at a moment
 * RV-9 can stop it. Ten minutes and then it gives up on its own, so a copy
 * started and forgotten does not outlive the afternoon.
 */
#include "modlib.h"

#define NAP_MS  100
#define NAPS    6000

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 2) return -1;

    for (uint32_t i = 0; i < NAPS; i++) {
        (void)env->signals_take();       /* heard, and ignored */
        env->sleep_ms(NAP_MS);
    }
    return 0;
}
