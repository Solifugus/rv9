/*
 * downloaded -- a program that was never flashed onto this board.
 *
 * Built by the host, excluded from the module store (see .nostore), served
 * over HTTP, written to a file by redirection and added to the module
 * directory by `load`. If it runs, a program reached the machine without
 * anyone reflashing it.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 3) return -1;

    m_say(env, RV9_STDOUT, "I was never flashed onto this board.\n");
    m_say(env, RV9_STDOUT, "I arrived over WiFi, through a file, as pid ");
    m_num(env, RV9_STDOUT, (int32_t)env->pid);
    m_say(env, RV9_STDOUT, ".\n");
    return 0;
}
