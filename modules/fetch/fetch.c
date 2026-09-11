/*
 * fetch -- an HTTP GET.  fetch example.com [path]
 *
 * The point is what is absent. No sockets, no DNS call, no connect: it
 * builds a path name, opens it, writes a request and reads the answer.
 * The same three verbs that read a file off /r0 or a line off /uart0.
 *
 * Headers go to stderr and the body to stdout, so that
 *
 *     fetch host /thing.mod > /r0/thing.mod
 *
 * puts exactly the bytes of the file on the volume while the status still
 * reaches the terminal. Redirection replaces stdout and leaves stderr
 * alone, which is what that separation is for.
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
        m_say(env, RV9_STDOUT, "usage: fetch <host>[:port] [path]\n");
        return -3;
    }

    char hostport[64];
    uint32_t used = word(hostport, sizeof(hostport), env->arg);

    /* "host" or "host:port". */
    char host[64];
    const char *port = "80";
    uint32_t i = 0;
    while (hostport[i] && hostport[i] != ':') { host[i] = hostport[i]; i++; }
    host[i] = '\0';
    if (hostport[i] == ':') port = &hostport[i + 1];

    const char *doc = env->arg + used;
    while (*doc == ' ') doc++;
    if (*doc == '\0') doc = "/";

    /* "/n0/<host>/80" -- the network as a path. */
    uint32_t at = append(st->path, 0, "/n0/", sizeof(st->path));
    at = append(st->path, at, host, sizeof(st->path));
    at = append(st->path, at, "/", sizeof(st->path));
    at = append(st->path, at, port, sizeof(st->path));

    m_say(env, RV9_STDERR, "opening ");
    m_say(env, RV9_STDERR, st->path);
    m_say(env, RV9_STDERR, "\n");

    int p = env->open(st->path, RV9_MODE_RW);
    if (p < 0) { m_say(env, RV9_STDERR, "cannot connect\n"); return -4; }

    uint32_t r = append(st->req, 0, "GET ", sizeof(st->req));
    r = append(st->req, r, doc, sizeof(st->req));
    r = append(st->req, r, " HTTP/1.0\r\nHost: ", sizeof(st->req));
    r = append(st->req, r, host, sizeof(st->req));
    r = append(st->req, r, "\r\nConnection: close\r\n\r\n", sizeof(st->req));

    if (env->write(p, st->req, r) < 0) {
        m_say(env, RV9_STDERR, "send failed\n");
        env->close(p);
        return -5;
    }

    /*
     * Split the reply at the blank line. Everything before it is status,
     * and goes to stderr; everything after is the file, and goes to
     * stdout, where redirection can put it somewhere useful.
     */
    int  body_bytes = 0;
    bool in_body = false;
    int  match = 0;              /* how much of "\r\n\r\n" has been seen */

    for (;;) {
        int n = env->read(p, st->buf, BUF_LEN);
        if (n <= 0) break;

        int start = 0;
        if (!in_body) {
            for (int i = 0; i < n; i++) {
                char c = st->buf[i];
                if ((match == 0 || match == 2) && c == '\r')      match++;
                else if ((match == 1 || match == 3) && c == '\n') match++;
                else match = (c == '\r') ? 1 : 0;

                if (match == 4) {
                    env->write(RV9_STDERR, st->buf, (uint32_t)(i + 1));
                    start = i + 1;
                    in_body = true;
                    break;
                }
            }
            if (!in_body) { env->write(RV9_STDERR, st->buf, (uint32_t)n); continue; }
        }

        if (n > start) {
            env->write(RV9_STDOUT, st->buf + start, (uint32_t)(n - start));
            body_bytes += n - start;
        }
    }

    env->close(p);

    m_say(env, RV9_STDERR, "--- ");
    m_num(env, RV9_STDERR, body_bytes);
    m_say(env, RV9_STDERR, " bytes of body\n");
    return body_bytes > 0 ? 0 : -6;
}
