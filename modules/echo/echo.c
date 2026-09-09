/*
 * echo -- proof that a command is just a module.
 *
 * Writes a line to stdout. It has no idea whether stdout is the serial
 * console, the panel, or something a future file manager invents, and it
 * inherited that path from whoever forked it without opening anything.
 */
#include "rv9/module.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)            return -1;
    if (env->abi_version < 3)   return -2;

    const char *msg = "echo: hello from a forked module\n";
    uint32_t n = 0;
    while (msg[n]) n++;

    return env->write(RV9_STDOUT, msg, n) >= 0 ? 0 : -3;
}
