/*
 * format -- make an empty RV-9 volume on a device.
 *
 *   format /sd0        say what it would destroy, and refuse
 *   format /sd0 yes    do it
 *
 * Two words, because there is no undo. A volume RV-9 does not recognise is
 * formatted when it is mounted -- a fresh card becomes RV-9 storage by
 * being put in -- so this is for the other case: a volume that already has
 * files on it, or one whose layout should be rebuilt (the root directory is
 * sized at format time from the size of the volume).
 *
 * On a large card this takes a while: the bitmap for 32 GB is fifteen
 * thousand sectors, and they are written one at a time.
 */
#include "modlib.h"

/* The file manager's own setting; see components/rv9_io/include/rv9/io.h. */
#define RV9_RBF_SS_FORMAT (RV9_SS_DRIVER_BASE + 64)

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: format <device> yes\n"
                               "  e.g. format /sd0 yes\n");
        return -2;
    }

    char dev[24];
    const char *rest = m_word(env->arg, dev, sizeof(dev));
    while (*rest == ' ') rest++;

    if (!m_eq(rest, "yes")) {
        m_say(env, RV9_STDOUT, "format: this empties ");
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, " completely, and there is no undo.\n"
                               "say so: format ");
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, " yes\n");
        return -3;
    }

    int p = env->open(dev, RV9_MODE_WRITE);
    if (p < 0) {
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, ": cannot open\n");
        return -4;
    }

    m_say(env, RV9_STDOUT, "formatting ");
    m_say(env, RV9_STDOUT, dev);
    m_say(env, RV9_STDOUT, " -- on a large card this takes a minute\n");

    int rc = env->setstat(p, RV9_RBF_SS_FORMAT, 0);
    env->close(p);

    if (rc < 0) {
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, ": not formatted (is it a volume?)\n");
        return -5;
    }

    m_say(env, RV9_STDOUT, dev);
    m_say(env, RV9_STDOUT, ": empty\n");
    return 0;
}
