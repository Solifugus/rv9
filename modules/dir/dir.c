/*
 * dir -- list files on a block device.
 *
 * Opening a device with no filename gives you its directory as a stream of
 * records. That is not a special case in the I/O manager; it falls out of a
 * directory being a file.
 */
#include "modlib.h"

#define MAX_ENTRIES 16

typedef struct {
    rv9_dirent_t ents[MAX_ENTRIES];
} dir_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 7) return -1;

    dir_statics_t *st = (dir_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    const char *dev = (env->arg && env->arg[0]) ? env->arg : "/r0";

    int p = env->open(dev, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, ": cannot open\n");
        return -3;
    }

    m_say(env, RV9_STDOUT, "name                     size\n");

    int total = 0;
    for (;;) {
        int got = env->read(p, st->ents, sizeof(st->ents));
        if (got <= 0) break;

        int count = got / (int)sizeof(rv9_dirent_t);
        for (int i = 0; i < count; i++) {
            m_pad(env, RV9_STDOUT, st->ents[i].name, 25);
            m_num(env, RV9_STDOUT, (int32_t)st->ents[i].size);
            m_say(env, RV9_STDOUT, "\n");
            total++;
        }
        if (count < MAX_ENTRIES) break;
    }

    env->close(p);

    if (total == 0) m_say(env, RV9_STDOUT, "(empty)\n");
    return 0;
}
