/*
 * info -- what a path is.
 *
 *   info /f0/prog.mod        /f0/prog.mod  788 bytes
 *   info /gpio/2             /gpio/2  a device, no size
 *
 * `dir` says this for every file on a volume at once; this says it for one
 * thing, including things `dir` will never list, because a device is a
 * path too and asking it how big it is a reasonable question with a
 * reasonable answer -- sometimes "that does not apply to me".
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    char name[48];
    name[0] = '\0';
    if (env->arg != NULL) m_word(env->arg, name, sizeof(name));

    if (name[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: info <path>\n");
        return -3;
    }

    int p = env->open(name, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDERR, name);
        m_say(env, RV9_STDERR, ": cannot open\n");
        return -4;
    }

    m_say(env, RV9_STDOUT, name);

    uint32_t size = 0;
    if (env->getstat(p, RV9_GS_SIZE, &size) == 0) {
        m_say(env, RV9_STDOUT, "  ");
        m_num(env, RV9_STDOUT, (int32_t)size);
        m_say(env, RV9_STDOUT, " bytes\n");
    } else {
        /* Not every path has a length. A pin does not, and saying "0"
           would be a measurement rather than the truth. */
        m_say(env, RV9_STDOUT, "  opens, and has no length\n");
    }

    env->close(p);
    return 0;
}
