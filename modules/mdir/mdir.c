/*
 * mdir -- list the module directory.
 *
 * A utility that is itself a module in the directory it prints, which is
 * the sort of thing that makes this worth building.
 */
#include "modlib.h"

/*
 * Enough for the whole store, which is the only number that is ever
 * right. It was sixteen, chosen when sixteen was generous; the store
 * passed eighty without anybody noticing, and `mdir` had been quietly
 * showing a fifth of it ever since. Piping it into `count` is what
 * finally said so out loud.
 */
#define MAX_MODULES 128

typedef struct {
    rv9_sys_module_t mods[MAX_MODULES];
} mdir_statics_t;

static const char *type_name(uint8_t t)
{
    switch (t) {
    case RV9_MOD_PROGRAM:    return "program";
    case RV9_MOD_LIBRARY:    return "library";
    case RV9_MOD_FILEMGR:    return "filemgr";
    case RV9_MOD_DRIVER:     return "driver";
    case RV9_MOD_DESCRIPTOR: return "descrip";
    case RV9_MOD_DATA:       return "data";
    case RV9_MOD_SYSTEM:     return "system";
    default:                 return "?";
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 5) return -1;

    mdir_statics_t *st = (mdir_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int n = env->sysinfo(RV9_SYS_MODULES, st->mods, sizeof(st->mods));
    if (n < 0) return -3;

    m_say(env, RV9_STDOUT, "name        type    rev  size link\n");

    for (int i = 0; i < n; i++) {
        m_pad(env, RV9_STDOUT, st->mods[i].name, 12);
        m_pad(env, RV9_STDOUT, type_name(st->mods[i].type), 8);
        m_numpad(env, RV9_STDOUT, st->mods[i].revision, 5);
        m_numpad(env, RV9_STDOUT, (int32_t)st->mods[i].size, 6);
        m_num(env, RV9_STDOUT, (int32_t)st->mods[i].links);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
