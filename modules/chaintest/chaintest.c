/*
 * chaintest -- becomes another module without becoming another process.
 *
 * Prints its pid, asks to chain to 'echo', and returns. The process keeps
 * its pid, its priority and its open paths; only the code changes. OS-9
 * called this F$Chain, and it is how a shell replaces itself with a
 * program instead of forking one.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 6) return -1;
    if (env->chain == NULL)                  return -2;

    m_say(env, RV9_STDOUT, "chaintest: running as pid ");
    m_num(env, RV9_STDOUT, (int32_t)env->pid);
    m_say(env, RV9_STDOUT, ", chaining to echo\n");

    if (env->chain("echo") < 0) {
        m_say(env, RV9_STDOUT, "chaintest: chain refused\n");
        return -3;
    }

    /* Returning is what lets the chain happen; we are not exec(). */
    return 0;
}
