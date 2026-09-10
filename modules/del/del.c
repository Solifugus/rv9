/*
 * del -- remove a file.  del /r0/notes.txt
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 7) return -1;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: del <path>\n");
        return -2;
    }

    if (env->remove(env->arg) < 0) {
        m_say(env, RV9_STDOUT, env->arg);
        m_say(env, RV9_STDOUT, ": cannot remove\n");
        return -3;
    }

    m_say(env, RV9_STDOUT, "removed ");
    m_say(env, RV9_STDOUT, env->arg);
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
