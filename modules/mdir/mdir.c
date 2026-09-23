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

    /* Thirteen wide because the longest name in the store is twelve
       ('st-rt-badpin') and a name that exactly fills its column used to
       fuse with the type beside it. The format allows 31, so m_pad still
       has to guarantee a separator; this only keeps the table straight. */
    m_say(env, RV9_STDOUT, "name         type    rev  size link\n");

    for (int i = 0; i < n; i++) {
        m_pad(env, RV9_STDOUT, st->mods[i].name, 13);
        m_pad(env, RV9_STDOUT, type_name(st->mods[i].type), 8);
        m_numpad(env, RV9_STDOUT, st->mods[i].revision, 5);
        m_numpad(env, RV9_STDOUT, (int32_t)st->mods[i].size, 6);
        m_num(env, RV9_STDOUT, (int32_t)st->mods[i].links);

        /*
         * The row's newline is also the test: if the reader has gone, stop.
         * `mdir | first 3` is a reasonable thing to type, and listing the
         * other hundred rows into a pipe nobody holds is work done for
         * nobody -- and, until pipefm latched its warning, a hundred
         * identical lines in the log.
         *
         * A zero-length write cannot be the test: pipefm returns OK for one
         * without looking at whether anybody is there.
         */
        if (m_say(env, RV9_STDOUT, "\n") < 0) break;
    }
    return 0;
}
