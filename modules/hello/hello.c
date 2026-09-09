/*
 * hello -- the first RV-9 module.
 *
 * Deliberately paranoid: it checks everything the module ABI promises,
 * because this module's real job is to prove the loader works. Later
 * modules can assume what this one verifies.
 *
 * Note what is absent: no libc, no globals, no imports. Everything comes
 * through env. The linker script refuses to link a module with .data or
 * .bss, so the compiler enforces the rule too.
 */
#include "rv9/module.h"

/* Per-instance state. The loader allocates and zeroes this; we never own
   storage in the module image itself. */
typedef struct {
    uint32_t runs;
    uint32_t signature;
} hello_statics_t;

#define HELLO_SIGNATURE 0x52563944u

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)                          return -1;
    if (env->abi_version < RV9_MODULE_ABI)    return -2;
    if (env->print == NULL)                   return -3;

    env->print("hello from a loaded RV-9 module");

    hello_statics_t *st = (hello_statics_t *)env->statics;
    if (st == NULL)                           return -4;
    if (env->statics_size < sizeof(*st))      return -5;

    /* The loader promises zeroed static storage. If this is not zero, either
       the loader lied or we are looking at another instance's memory. */
    if (st->runs != 0 || st->signature != 0)  return -6;

    st->runs = 1;
    st->signature = HELLO_SIGNATURE;
    if (st->signature != HELLO_SIGNATURE)     return -7;

    if (env->time_ms == NULL)                 return -8;
    if (env->time_ms() == 0)                  return -9;

    env->print("statics are zeroed and writable, callbacks live");

    return 42;
}
