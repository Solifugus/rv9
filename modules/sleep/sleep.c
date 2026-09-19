/*
 * sleep -- wait a while.
 *
 *   sleep 2          two seconds
 *   sleep 250ms      a quarter of one
 *
 * Exists for scripts, which cannot otherwise wait for anything: a device
 * to settle, a loop to publish, a session to finish. Seconds by default
 * because that is what somebody typing it means, milliseconds when asked
 * for because that is what a control system means.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;
    if (env->sleep_ms == NULL) return -2;

    if (env->arg == NULL || env->arg[0] < '0' || env->arg[0] > '9') {
        m_say(env, RV9_STDERR, "usage: sleep <n> or sleep <n>ms\n");
        return -3;
    }

    const char *end = env->arg;
    uint32_t    n   = m_num_parse(env->arg, &end);

    bool ms = (end[0] == 'm' && end[1] == 's');
    uint32_t wait = ms ? n : n * 1000u;

    /* A whole minute is almost always a typo for a second, and a shell
       that will not come back is indistinguishable from a broken one. */
    if (wait > 60000u) {
        m_say(env, RV9_STDERR, "sleep: more than a minute; say it in ms if "
                               "you meant it\n");
        return -4;
    }

    env->sleep_ms(wait);
    return 0;
}
