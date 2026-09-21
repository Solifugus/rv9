/*
 * echo -- write the rest of the line to standard output.
 *
 * It has no idea whether stdout is the serial console, the panel, a pipe or
 * a file: it inherited that path from whoever forked it, without opening
 * anything. That was the original point of this module, which said one
 * fixed sentence to prove a command is just a module.
 *
 * It now says what it was told. The proof still stands and the command is
 * useful, which are not in tension -- and one thing in particular depended
 * on it: with `>>` in the shell, `echo` is how a file gets written on a
 * board with no editor worth using, and therefore how a script gets
 * written. A command called `echo` that ignored its argument was fine
 * while nothing could be composed and quietly absurd afterwards.
 *
 * No escape sequences and no options. A newline is always added, because
 * the one thing every caller wants is a line.
 */
#include "rv9/module.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)            return -1;
    if (env->abi_version < 8)   return -2;    /* env->arg */

    const char *msg = (env->arg != NULL) ? env->arg : "";

    uint32_t n = 0;
    while (msg[n]) n++;

    if (n > 0 && env->write(RV9_STDOUT, msg, n) < 0) return 3;
    return env->write(RV9_STDOUT, "\n", 1) >= 0 ? 0 : 3;
}
