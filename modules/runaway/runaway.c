/*
 * runaway -- a real-time loop that stops waiting.
 *
 *   rt runaway            on time for 20 periods, then never waits again
 *   rt runaway syscalls   the same, but spinning through system calls
 *
 * It declares nothing about deadlines, so a late period is only counted.
 * That is not leave to take the machine: at the real-time priority a loop
 * that stops waiting starves everything under it, including the shell
 * somebody would type `kill` into, and it never reaches the one call where
 * RV-9 could stop it politely. So it is found by the watchdog, and stopped
 * from outside -- with /gpio/2 parked at 0 afterwards, as for any exit.
 *
 * `syscalls` is the harder case. A loop spinning through the I/O manager
 * is, at any instant, quite likely to be inside it and holding a lock, and
 * suspending it there could hang whatever wants the lock next. RV-9 lowers
 * it and waits for an instant when it is in its own code.
 */
#include "modlib.h"

#define PIN      "/gpio/2"
#define ON_TIME  20

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;
    if (env->rt_declare == NULL || env->rt_wait == NULL) {
        m_say(env, RV9_STDOUT, "runaway: not a real-time process\n"
                               "  run it with: rt runaway [syscalls]\n");
        return -2;
    }

    bool through_calls = (env->arg != NULL && m_eq(env->arg, "syscalls"));

    int pin = env->open(PIN, RV9_MODE_RW);
    if (pin < 0) {
        m_say(env, RV9_STDOUT, "runaway: cannot open " PIN "\n");
        return -3;
    }
    uint32_t v = 1;
    env->write(pin, &v, sizeof(v));

    if (env->rt_declare(0) < 0) {
        env->close(pin);
        return -4;
    }

    for (uint32_t n = 0; n < ON_TIME; n++) {
        if (env->rt_wait() < 0) break;
    }

    if (through_calls) {
        for (;;) env->getstat(pin, RV9_GS_READY, &v);
    }
    for (;;) { }
}
