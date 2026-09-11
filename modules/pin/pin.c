/*
 * pin -- read or set a GPIO.   pin 3        pin 3 1
 *
 * The device carries binary values, so this converts text for the human at
 * the shell. A control loop skips the conversion and writes the value.
 *
 * A pin keeps its level after this exits. That is deliberate and it is the
 * opposite of what /pwm0 does, so it is worth being explicit about both:
 *
 *   /gpio   holds its level when the path closes. Setting an enable line
 *           and having it drop when the command finished would be useless.
 *   /pwm0   stops driving when the path closes, because it holds a
 *           hardware channel that must be given back -- and an actuator
 *           still running because a program exited is a bad surprise.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;
    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: pin <n> [0|1]\n");
        return -2;
    }

    char num[8];
    const char *rest = m_word(env->arg, num, sizeof(num));

    char path[24];
    uint32_t at = 0;
    const char *pre = "/gpio/";
    while (pre[at]) { path[at] = pre[at]; at++; }
    for (uint32_t i = 0; num[i]; i++) path[at++] = num[i];
    path[at] = '\0';

    bool writing = (*rest != '\0');

    /*
     * Read-write when setting, so the level can be read back.
     *
     * Opening write-only and then reading gets refused by the I/O manager,
     * exactly as it should -- and ignoring that refusal made the driver
     * look broken for a while: every write reported a mismatch because the
     * read never happened and the variable printed was the one that had
     * been initialised to zero.
     */
    int p = env->open(path, writing ? RV9_MODE_RW : RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDOUT, path);
        m_say(env, RV9_STDOUT, ": cannot open (reserved or not a pin)\n");
        return -3;
    }

    int rc = 0;
    if (writing) {
        uint32_t v = m_num_parse(rest, 0) ? 1 : 0;
        if (env->write(p, &v, sizeof(v)) < 0) {
            rc = -4;
        } else {
            /* Read it back. A pin opened for writing is configured
               input-output, so the level can be confirmed rather than
               assumed -- worth doing when the thing on the other end
               moves. */
            uint32_t back = 0;
            int got = env->read(p, &back, sizeof(back));

            m_say(env, RV9_STDOUT, path);
            m_say(env, RV9_STDOUT, " := ");
            m_num(env, RV9_STDOUT, (int32_t)v);

            if (got < 0) {
                /* Say so, rather than printing a value nobody produced. */
                m_say(env, RV9_STDOUT, " (could not read back)\n");
            } else {
                m_say(env, RV9_STDOUT, ", reads ");
                m_num(env, RV9_STDOUT, (int32_t)back);
                m_say(env, RV9_STDOUT, back == v ? "\n" : "  (MISMATCH)\n");
            }
        }
    } else {
        uint32_t v = 0;
        if (env->read(p, &v, sizeof(v)) < 0) {
            rc = -5;
        } else {
            m_say(env, RV9_STDOUT, path);
            m_say(env, RV9_STDOUT, " = ");
            m_num(env, RV9_STDOUT, (int32_t)v);
            m_say(env, RV9_STDOUT, "\n");
        }
    }

    env->close(p);
    return rc;
}
