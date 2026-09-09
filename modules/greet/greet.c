/*
 * greet -- writes through the full I/O stack.
 *
 * Two routes, both ending at hardware through path -> file manager ->
 * driver -> descriptor:
 *
 *   1. its inherited stdout, which it never had to open
 *   2. /term, opened by name, with no idea what is behind that name
 *
 * The module contains no knowledge of ST7789, SPI, or line endings. That
 * is the entire argument for the layering.
 */
#include "rv9/module.h"

/* No libc in a module. */
static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int say(const rv9_mod_env_t *env, int path, const char *s)
{
    return env->write(path, s, slen(s));
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)                        return -1;
    if (env->abi_version < 3)               return -2;
    if (env->open == NULL || env->write == NULL) return -3;

    /* Inherited stdout: this module never opened anything. */
    say(env, RV9_STDOUT, "greet: writing to inherited stdout\n");

    /* And a device opened by name. */
    int term = env->open("/term", RV9_MODE_WRITE);
    if (term < 0) return -4;

    say(env, term, "\n");
    say(env, term, "  RV-9 greet module\n");
    say(env, term, "  path -> scf -> lcdcon\n");
    say(env, term, "  no ST7789 knowledge here\n");

    if (env->close(term) < 0) return -5;

    say(env, RV9_STDOUT, "greet: wrote to /term and closed it\n");
    return 0;
}
