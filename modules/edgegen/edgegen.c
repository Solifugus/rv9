/*
 * edgegen -- toggle a pin, so that something else has edges to react to.
 *
 *     edgegen <pin> [count] [interval_ms]
 *
 * Deliberately an ordinary process, not a real-time one. It is the world:
 * sloppy, scheduled whenever, running at whatever priority it was given.
 * That is the right shape for the far end of a latency measurement, because
 * the number under test is how quickly the system responds to an edge, and
 * the ISR stamps the edge itself -- nothing upstream of that stamp can
 * flatter the result.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 7) return -1;
    if (env->arg == NULL) {
        m_say(env, RV9_STDOUT, "usage: edgegen <pin> [count] [interval_ms]\n");
        return -2;
    }

    const char *s = env->arg;
    uint32_t pin      = m_num_parse(s, &s);
    while (*s == ' ') s++;
    uint32_t count    = m_num_parse(s, &s);
    while (*s == ' ') s++;
    uint32_t interval = m_num_parse(s, &s);

    if (count == 0)    count = 600;
    if (interval == 0) interval = 2;

    char name[24];
    m_devpath(name, "/gpio/", pin);

    int p = env->open(name, RV9_MODE_RW);
    if (p < 0) {
        m_say(env, RV9_STDOUT, "edgegen: cannot open ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, "\n");
        return -3;
    }

    /*
     * Let whoever is measuring get armed first. Opening a pin no longer
     * disturbs another process's configuration of it -- the driver keeps
     * count -- but an edge produced before the listener is waiting is an
     * edge nobody asked about, and it would be counted as coalesced.
     */
    env->sleep_ms(200);

    for (uint32_t n = 0; n < count; n++) {
        uint32_t level = n & 1;
        env->write(p, &level, sizeof(level));
        env->sleep_ms(interval);

        if (env->signals_take() & RV9_SIG_STOP) break;
    }

    env->close(p);
    return 0;
}
