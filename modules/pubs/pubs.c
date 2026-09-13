/*
 * pubs -- what is being published, and by whom.
 *
 * The measurement side of publication, in the same spirit as `owns`,
 * `procs` and `stacks`: a mechanism nobody can look at is a mechanism
 * nobody can debug. `dir /pub0` already lists the cells, because a cell
 * store is a directory; this adds the parts that make a publication a
 * publication -- how many times it has been published, how long ago the
 * value was observed, who is publishing it, and how often an observer was
 * outrun trying to read it.
 *
 * The last column is the one worth watching. A non-zero `torn` means some
 * reader could not take a coherent snapshot between two publications, and
 * that is a real property of the system rather than a fault in the reader.
 */
#include "modlib.h"

#define DEV         "/pub0"
#define MAX_ENTRIES 16

typedef struct {
    rv9_dirent_t ents[MAX_ENTRIES];
    char         path[48];
} pubs_statics_t;

static void build_path(char *out, uint32_t cap, const char *name)
{
    const char *dev = DEV;
    uint32_t i = 0;
    while (*dev && i < cap - 2) out[i++] = *dev++;
    out[i++] = '/';
    for (uint32_t k = 0; name[k] && i < cap - 1; k++) out[i++] = name[k];
    out[i] = '\0';
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 11) return -1;

    pubs_statics_t *st = (pubs_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int d = env->open(DEV, RV9_MODE_READ);
    if (d < 0) {
        m_say(env, RV9_STDOUT, "pubs: this system publishes nothing "
                               "(no /pub0)\n");
        return -3;
    }

    int got = env->read(d, st->ents, sizeof(st->ents));
    env->close(d);

    int count = (got > 0) ? got / (int)sizeof(rv9_dirent_t) : 0;
    if (count == 0) {
        m_say(env, RV9_STDOUT, "no cells in use\n");
        return 0;
    }

    m_say(env, RV9_STDOUT,
          "name                 seq   bytes   cap  age_ms  by   rdrs  torn\n");

    uint64_t now = env->time_us();

    for (int i = 0; i < count; i++) {
        build_path(st->path, sizeof(st->path), st->ents[i].name);

        int p = env->open(st->path, RV9_MODE_READ);
        if (p < 0) continue;

        rv9_pub_info_t info;
        int r = env->getstat(p, RV9_PUB_GS_INFO, &info);
        env->close(p);
        if (r < 0) continue;

        m_pad(env, RV9_STDOUT, st->ents[i].name, 21);
        m_numpad(env, RV9_STDOUT, (int32_t)info.seq, 6);
        m_numpad(env, RV9_STDOUT, (int32_t)info.len, 8);
        m_numpad(env, RV9_STDOUT, (int32_t)info.cap, 5);

        /*
         * Never published reads as a dash rather than as an age, because
         * zero would be a lie in the most misleading possible direction:
         * a supervisor cannot tell "fresh" from "there has never been
         * one" if both print the same number. R9 §18 calls this the
         * validity indication, and this is where it becomes visible.
         */
        if (info.seq == 0) {
            m_pad(env, RV9_STDOUT, "-", 8);
        } else {
            m_numpad(env, RV9_STDOUT, (int32_t)m_age_ms(now, info.stamp_us), 8);
        }

        /*
         * Not held reads as a dash; held by pid 0 reads as "sys", because
         * that is RV-9 itself publishing and not an absence.
         */
        if (!info.held)            m_pad(env, RV9_STDOUT, "-", 5);
        else if (info.writer == 0) m_pad(env, RV9_STDOUT, "sys", 5);
        else m_numpad(env, RV9_STDOUT, (int32_t)info.writer, 5);

        /* The reader count is one higher than the truth while this is
           looking: pubs has the cell open to ask. Say the honest number. */
        m_numpad(env, RV9_STDOUT,
                 (int32_t)((info.readers > 0) ? info.readers - 1 : 0), 6);
        m_numpad(env, RV9_STDOUT, (int32_t)info.torn, 6);
        m_say(env, RV9_STDOUT, "\n");
    }

    return 0;
}
