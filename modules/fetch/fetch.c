/*
 * fetch -- an HTTP GET.  fetch example.com [path]
 *
 * The point is what is absent. No sockets, no DNS call, no connect: it
 * builds a path name, opens it, writes a request and reads the answer.
 * The same three verbs that read a file off /r0 or a line off /uart0.
 */
#include "modlib.h"

#define BUF_LEN 1024

typedef struct {
    char path[96];
    char req[160];
    char buf[BUF_LEN];
} fetch_statics_t;

static uint32_t append(char *dst, uint32_t at, const char *src, uint32_t cap)
{
    while (*src && at < cap - 1) dst[at++] = *src++;
    dst[at] = '\0';
    return at;
}

/* Copy up to the first space; returns where it stopped in src. */
static uint32_t word(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    while (src[i] && src[i] != ' ' && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return -1;

    fetch_statics_t *st = (fetch_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: fetch <host> [path]\n");
        return -3;
    }

    char host[64];
    uint32_t used = word(host, sizeof(host), env->arg);

    const char *doc = env->arg + used;
    while (*doc == ' ') doc++;
    if (*doc == '\0') doc = "/";

    /* "/n0/<host>/80" -- the network as a path. */
    uint32_t at = append(st->path, 0, "/n0/", sizeof(st->path));
    at = append(st->path, at, host, sizeof(st->path));
    at = append(st->path, at, "/80", sizeof(st->path));

    m_say(env, RV9_STDOUT, "opening ");
    m_say(env, RV9_STDOUT, st->path);
    m_say(env, RV9_STDOUT, "\n");

    int p = env->open(st->path, RV9_MODE_RW);
    if (p < 0) { m_say(env, RV9_STDOUT, "cannot connect\n"); return -4; }

    uint32_t r = append(st->req, 0, "GET ", sizeof(st->req));
    r = append(st->req, r, doc, sizeof(st->req));
    r = append(st->req, r, " HTTP/1.0\r\nHost: ", sizeof(st->req));
    r = append(st->req, r, host, sizeof(st->req));
    r = append(st->req, r, "\r\nConnection: close\r\n\r\n", sizeof(st->req));

    if (env->write(p, st->req, r) < 0) {
        m_say(env, RV9_STDOUT, "send failed\n");
        env->close(p);
        return -5;
    }

    int total = 0;
    for (;;) {
        int n = env->read(p, st->buf, BUF_LEN - 1);
        if (n <= 0) break;
        st->buf[n] = '\0';
        if (total == 0) m_say(env, RV9_STDOUT, st->buf);   /* headers */
        total += n;
        if (total > 8192) break;
    }

    env->close(p);

    m_say(env, RV9_STDOUT, "\n--- ");
    m_num(env, RV9_STDOUT, total);
    m_say(env, RV9_STDOUT, " bytes\n");
    return total > 0 ? 0 : -6;
}
