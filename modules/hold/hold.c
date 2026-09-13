/*
 * hold -- keep the devices this program declared it must own, and prove
 * RV-9 takes them back and parks them when it stops.
 *
 * The manifest declares two exclusive devices and one failsafe:
 *
 *     exclusives="/pwm0/3 /gpio/2"
 *     failsafes="/gpio/2=0"
 *
 * so RV-9 claims both at fork, before this code runs, and writes 0 to
 * /gpio/2 when the process ends however it ends. Four things follow, and
 * all four are visible from the shell:
 *
 *   a second `hold` never starts -- refused at fork, not partway through;
 *   `pwm 3 1200` and `pin 2` are refused at open;
 *   `owns` shows both devices, with the promise against the pin;
 *   `hold crash` dies of a stack overflow with the pin driven high, and
 *   `pin 2` afterwards reads 0.
 *
 * The last one is the whole point. A program that ends by returning lets
 * go of things because it is asked to. A program stopped by the scheduler
 * is not asked anything -- so the parking cannot be code in this module,
 * and it is not: it is a number and a device name in the manifest, applied
 * by RV-9 with this process already dead.
 *
 * The pin is driven high on purpose, so that 0 afterwards is evidence
 * rather than the value it happened to have anyway.
 *
 *   hold          take them and keep them, until stopped or time runs out
 *   hold crash    take them, drive the pin, and die badly holding them
 *
 * /pwm0/3 is here for the contrast: it does not hold its state when
 * released, so admission warns that its failsafe would not outlive it and
 * none is declared. Its driver stopping the channel is its own safe state.
 */
#include "modlib.h"

#define PWM      "/pwm0/3"
#define PIN      "/gpio/2"
#define HOLD_MS  100
#define HOLD_MAX 300          /* thirty seconds, then give them back */

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    int pw = env->open(PWM, RV9_MODE_WRITE);
    if (pw < 0) {
        /* Should not happen: the fork-time reservation already proved the
           device exists and is ours. Say so plainly if it does. */
        m_say(env, RV9_STDOUT, PWM ": cannot open (");
        m_num(env, RV9_STDOUT, pw);
        m_say(env, RV9_STDOUT, ")\n");
        return -2;
    }

    int pn = env->open(PIN, RV9_MODE_RW);
    if (pn < 0) {
        m_say(env, RV9_STDOUT, PIN ": cannot open (");
        m_num(env, RV9_STDOUT, pn);
        m_say(env, RV9_STDOUT, ")\n");
        env->close(pw);
        return -2;
    }

    /* Drive it high, so that the failsafe having run is measurable. */
    uint32_t one = 1;
    env->write(pn, &one, sizeof(one));

    m_say(env, RV9_STDOUT, "holding " PWM " and " PIN " as pid ");
    m_num(env, RV9_STDOUT, (int32_t)env->pid);
    m_say(env, RV9_STDOUT, "\n" PIN " is now 1; RV-9 owes it a 0\n");

    if (env->arg != NULL && m_eq(env->arg, "crash")) {
        m_say(env, RV9_STDOUT, "now dying without letting go...\n");
        if (env->chain("smash") < 0) {
            m_say(env, RV9_STDOUT, "cannot chain to smash\n");
            env->close(pn);
            env->close(pw);
            return -3;
        }
        return 0;      /* the chain happens when this returns */
    }

    for (uint32_t i = 0; i < HOLD_MAX; i++) {
        if (env->signals_take() & RV9_SIG_STOP) break;
        env->sleep_ms(HOLD_MS);
    }

    env->close(pn);
    env->close(pw);
    m_say(env, RV9_STDOUT, "let go\n");
    return 0;
}
