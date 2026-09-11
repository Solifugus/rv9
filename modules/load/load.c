/*
 * load -- add a program to the system at runtime.  load /r0/thing.mod
 *
 * The module store stops being something you reflash and becomes something
 * you add to. Combined with redirection into a file, and a network that is
 * just another path, a program can arrive from anywhere.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: load <path>\n");
        return -2;
    }

    int rc = env->load(env->arg);
    if (rc < 0) {
        m_say(env, RV9_STDOUT, env->arg);
        m_say(env, RV9_STDOUT, ": not a loadable module (");
        m_num(env, RV9_STDOUT, rc);
        m_say(env, RV9_STDOUT, ")\n");
        return -3;
    }

    m_say(env, RV9_STDOUT, "loaded; 'mdir' lists it\n");
    return 0;
}
