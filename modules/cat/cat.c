/*
 * cat -- copy a file to stdout.
 *
 *   cat /f0/notes
 *   cat /f0/picture.svg > /w0
 *
 * Missing until now because nothing had needed it: a file was something
 * you loaded as a module or edited. The second form is the point -- with
 * redirection already working, a file reaching a device needs no new verb.
 */
#include "modlib.h"

#define CHUNK 256

typedef struct { char buf[CHUNK]; } cat_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    cat_statics_t *st = (cat_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0' || env->arg[0] == ' ') {
        m_say(env, RV9_STDERR, "usage: cat <file>\n");
        return -3;
    }

    char name[48];
    m_word(env->arg, name, sizeof(name));

    int p = env->open(name, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDERR, name);
        m_say(env, RV9_STDERR, ": cannot open\n");
        return -4;
    }

    for (;;) {
        int n = env->read(p, st->buf, CHUNK);
        if (n <= 0) break;
        env->write(RV9_STDOUT, st->buf, (uint32_t)n);
    }

    env->close(p);
    return 0;
}
