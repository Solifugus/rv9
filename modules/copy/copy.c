/*
 * copy -- one file into another.
 *
 *   copy /r0/notes /f0/notes        scratch onto flash, so it survives
 *   copy /f0/prog.mod /sd0/prog.mod
 *
 * Between volumes as readily as within one, because a volume is a device
 * and this only ever opens two paths. The destination is created, and an
 * existing one is replaced -- which is what "copy this over that" means
 * everywhere else and would be a surprise if it did not.
 */
#include "modlib.h"

#define CHUNK 256

typedef struct { char buf[CHUNK]; } copy_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    copy_statics_t *st = (copy_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "copy: static_size in build.conf is too small\n");
        return -2;
    }

    char from[48], to[48];
    const char *rest = (env->arg != NULL) ? m_word(env->arg, from, sizeof(from))
                                          : "";
    m_word(rest, to, sizeof(to));

    if (from[0] == '\0' || to[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: copy <from> <to>\n");
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

    int32_t total = 0;
    for (;;) {
        int n = env->read(src, st->buf, CHUNK);
        if (n <= 0) break;

        int w = env->write(dst, st->buf, (uint32_t)n);
        if (w < n) {
            /* A short write is the volume filling up, and stopping quietly
               would leave a file that looks finished and is not. */
            m_say(env, RV9_STDERR, to);
            m_say(env, RV9_STDERR, ": ran out of room\n");
            env->close(src);
            env->close(dst);
            return -6;
        }
        total += n;
    }

    env->close(src);
    env->close(dst);

    m_num(env, RV9_STDOUT, total);
    m_say(env, RV9_STDOUT, " bytes copied\n");
    return 0;
}
