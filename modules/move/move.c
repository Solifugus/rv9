/*
 * move -- a file, somewhere else.
 *
 *   move /r0/draft /f0/draft
 *
 * Copy then remove, which is the only honest implementation here: RBF has
 * no rename, and across two volumes there is nothing to rename anyway.
 * The original is removed only after the copy has finished and closed, so
 * an interrupted move loses nothing -- it leaves both, which is the
 * failure worth having.
 */
#include "modlib.h"

#define CHUNK 256

typedef struct { char buf[CHUNK]; } move_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    move_statics_t *st = (move_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "move: static_size in build.conf is too small\n");
        return -2;
    }

    char from[48], to[48];
    const char *rest = (env->arg != NULL) ? m_word(env->arg, from, sizeof(from))
                                          : "";
    m_word(rest, to, sizeof(to));

    if (from[0] == '\0' || to[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: move <from> <to>\n");
        return -3;
    }

    int src = env->open(from, RV9_MODE_READ);
    if (src < 0) {
        m_say(env, RV9_STDERR, from);
        m_say(env, RV9_STDERR, ": cannot open\n");
        return -4;
    }

    int dst = env->open(to, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (dst < 0) {
        m_say(env, RV9_STDERR, to);
        m_say(env, RV9_STDERR, ": cannot create\n");
        env->close(src);
        return -5;
    }

    bool ok = true;
    for (;;) {
        int n = env->read(src, st->buf, CHUNK);
        if (n <= 0) break;
        if (env->write(dst, st->buf, (uint32_t)n) < n) { ok = false; break; }
    }

    env->close(src);
    env->close(dst);

    if (!ok) {
        m_say(env, RV9_STDERR, to);
        m_say(env, RV9_STDERR, ": ran out of room; the original is untouched\n");
        return -6;
    }

    if (env->remove(from) < 0) {
        m_say(env, RV9_STDERR, from);
        m_say(env, RV9_STDERR, ": copied, but the original could not be "
                               "removed\n");
        return -7;
    }
    return 0;
}
