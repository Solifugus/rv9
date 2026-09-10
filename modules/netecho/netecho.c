/*
 * netecho -- accept one connection, echo one message back, exit.
 *
 * Forked by nettest with the port as its argument. It contains no sockets,
 * no bind, no accept: it opens a path and reads it.
 */
#include "modlib.h"

#define BUF_LEN 128

typedef struct { char buf[BUF_LEN]; char path[48]; } netecho_statics_t;

static void build_path(char *out, const char *port)
{
    const char *prefix = "/n0/listen/";
    uint32_t i = 0;
    while (prefix[i]) { out[i] = prefix[i]; i++; }
    uint32_t j = 0;
    while (port[j] && i < 46) out[i++] = port[j++];
    out[i] = '\0';
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return -1;

    netecho_statics_t *st = (netecho_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;
    if (env->arg == NULL || env->arg[0] == '\0') return -3;

    build_path(st->path, env->arg);

    m_say(env, RV9_STDOUT, "netecho: listening on ");
    m_say(env, RV9_STDOUT, st->path);
    m_say(env, RV9_STDOUT, "\n");

    /* Blocks until someone connects. Opening a path that is not ready yet
       is not a network-specific idea. */
    int p = env->open(st->path, RV9_MODE_RW);
    if (p < 0) return -4;

    int n = env->read(p, st->buf, BUF_LEN);
    if (n > 0) env->write(p, st->buf, n);

    env->close(p);
    return n > 0 ? 0 : -5;
}
