/*
 * hold -- keep a device this program declared it must own.
 *
 * The manifest says `exclusive=/pwm0/3`, so RV-9 claims that output at
 * fork, before this code has run. Two consequences worth watching:
 *
 *   a second `hold` never starts. It is refused at fork with "a device it
 *   needs alone is owned", not partway through, and not after it has
 *   configured a second LEDC channel onto the same pin.
 *
 *   `pwm 3 1200` is refused at open, with RV9_IOE_BUSY. The pin is spoken
 *   for by a program that has declared responsibility for it.
 *
 * A PWM output is the right thing to demonstrate this on. Two processes
 * opening /pwm0/3 today each get their own hardware channel, both wired to
 * the same pin, and neither the driver nor the hardware has any way to
 * prefer one -- the output simply does whatever the last writer said.
 *
 *   hold          take it and keep it, until stopped or the time runs out
 *   hold crash    take it and then die badly, holding it
 *
 * The second is the case that matters. A program that ends by returning
 * lets go of things because it is asked to; a program stopped by the
 * scheduler is not asked anything. So `hold crash` chains to `smash`,
 * which runs off its stack and is killed -- and `owns` afterwards must
 * show /pwm0/3 free. Dying is not a way to keep the motor.
 */
#include "modlib.h"

#define DEVICE   "/pwm0/3"
#define HOLD_MS  100
#define HOLD_MAX 300          /* thirty seconds, then give it back */

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    int p = env->open(DEVICE, RV9_MODE_WRITE);
    if (p < 0) {
        /* Should not happen: the fork-time reservation already proved the
           device exists and is ours. Say so plainly if it does. */
        m_say(env, RV9_STDOUT, DEVICE ": cannot open (");
        m_num(env, RV9_STDOUT, p);
        m_say(env, RV9_STDOUT, ")\n");
        return -2;
    }

    m_say(env, RV9_STDOUT, "holding " DEVICE " as pid ");
    m_num(env, RV9_STDOUT, (int32_t)env->pid);
    m_say(env, RV9_STDOUT, "\n");

    if (env->arg != NULL && m_eq(env->arg, "crash")) {
        m_say(env, RV9_STDOUT, "now dying without letting go...\n");
        if (env->chain("smash") < 0) {
            m_say(env, RV9_STDOUT, "cannot chain to smash\n");
            env->close(p);
            return -3;
        }
        return 0;      /* the chain happens when this returns */
    }

    for (uint32_t i = 0; i < HOLD_MAX; i++) {
        if (env->signals_take() & RV9_SIG_STOP) break;
        env->sleep_ms(HOLD_MS);
    }

    env->close(p);
    m_say(env, RV9_STDOUT, "let go of " DEVICE "\n");
    return 0;
}
