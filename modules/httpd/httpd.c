/*
 * httpd -- this machine's modules, served to its peers.
 *
 *   GET /catalog           one line per module /m0 holds:
 *                          name revision size type signer sha256
 *   GET /cmd/<name>.mod    the module, byte for byte from /m0
 *   GET /files/<name>      /sd0/<name>, if a module here declares it
 *
 * Read-only, HTTP/1.0, one request a connection, one connection at a time.
 * It is what the other end of a module path on a peer reads (design §74),
 * and the same layout a PC serves a release in, so a board and a publisher
 * look alike to whoever fetches.
 *
 * WHAT IT WILL NOT SERVE
 *
 * Anything not named above. A file is served only when some module this
 * machine holds says it reads it (its `file` tags): a model or a voice,
 * which are other people's published work, and never what a program keeps
 * about the people in front of it -- detect's /sd0/faces.db is on the same
 * card and is not anybody's to fetch.
 *
 * No authentication, because nothing here is secret: modules are signed,
 * and a peer checks the signature, not the server. Started by init when
 * /f0/serve exists; the port is the number in it, else 8080.
 */
#include "modlib.h"

#define SERVE_FILE   "/f0/serve"
#define DEFAULT_PORT 8080
#define NEEDS_MAX    1024

typedef struct {
    char req[512];
    uint8_t buf[1024];
    rv9_dirent_t ents[8];
    char path[48];
    char listen[32];
    rv9_sys_needs_t n;
    uint8_t more[NEEDS_MAX];
    rv9_sys_check_t ck;
    uint32_t served;
} httpd_statics_t;

static void put(const rv9_mod_env_t *env, int c, const char *s) { m_say(env, c, s); }

static void status(const rv9_mod_env_t *env, int c, const char *code, int64_t len)
{
    put(env, c, "HTTP/1.0 ");
    put(env, c, code);
    put(env, c, "\r\nServer: rv9-httpd\r\nConnection: close\r\n");
    if (len >= 0) {
        put(env, c, "Content-Length: ");
        m_num(env, c, (int32_t)len);
        put(env, c, "\r\n");
    }
    put(env, c, "\r\n");
}

static void not_found(const rv9_mod_env_t *env, int c)
{
    status(env, c, "404 Not Found", 10);
    put(env, c, "not here\r\n");
}

/* A name with nothing in it that could climb out of where it is looked up. */
static bool plain_name(const char *s)
{
    if (s[0] == '\0' || s[0] == '.') return false;
    for (; *s; s++) {
        char ch = *s;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

static bool starts(const char *s, const char *pre)
{
    while (*pre) if (*s++ != *pre++) return false;
    return true;
}

static void copy_str(char *dst, uint32_t cap, const char *a, const char *b)
{
    uint32_t i = 0;
    while (*a && i < cap - 1) dst[i++] = *a++;
    while (b && *b && i < cap - 1) dst[i++] = *b++;
    dst[i] = '\0';
}

/* Send a whole file, with its length. */
static void send_file(const rv9_mod_env_t *env, httpd_statics_t *st, int c, const char *file)
{
    int p = env->open(file, RV9_MODE_READ);
    if (p < 0) { not_found(env, c); return; }
    uint64_t size = 0;
    if (env->getstat(p, RV9_GS_SIZE, &size) < 0) size = 0;
    status(env, c, "200 OK", (int64_t)size);
    for (;;) {
        int n = env->read(p, st->buf, sizeof(st->buf));
        if (n <= 0) break;
        if (env->write(c, st->buf, (uint32_t)n) != n) break;
    }
    env->close(p);
    st->served++;
}

/* One catalog line from a module's header and, if signed, its block. */
static void catalog_line(const rv9_mod_env_t *env, httpd_statics_t *st, int c,
                         const rv9_dirent_t *d)
{
    char name[29];
    copy_str(name, sizeof(name), d->name, NULL);
    copy_str(st->path, sizeof(st->path), "/m0/", name);
    int p = env->open(st->path, RV9_MODE_READ);
    if (p < 0) return;
    rv9_mod_header_t h;
    if (env->read(p, &h, sizeof(h)) != (int)sizeof(h)) { env->close(p); return; }

    const char *type = h.type == RV9_MOD_PROGRAM ? "program"
                     : h.type == RV9_MOD_DESCRIPTOR ? "descriptor" : "other";
    put(env, c, name);
    put(env, c, " ");
    m_num(env, c, h.revision);
    put(env, c, " ");
    m_num(env, c, (int32_t)d->size);
    put(env, c, " ");
    put(env, c, type);
    put(env, c, " ");

    rv9_mod_sig_t b;
    if ((h.attr & RV9_MOD_ATTR_SIGNED) && d->size > sizeof(b) &&
        env->seek(p, (int32_t)(d->size - sizeof(b)), RV9_SEEK_SET) >= 0 &&
        env->read(p, &b, sizeof(b)) == (int)sizeof(b) && b.magic == RV9_MOD_SIG_MAGIC) {
        char hex[17];
        for (int i = 0; i < 8; i++) {
            hex[2 * i] = "0123456789abcdef"[b.key_id[i] >> 4];
            hex[2 * i + 1] = "0123456789abcdef"[b.key_id[i] & 15];
        }
        hex[16] = '\0';
        put(env, c, hex);
    } else {
        put(env, c, "-");
    }
    env->close(p);

    /* The file's SHA-256, which is what /f0/pins names a build by. */
    put(env, c, " ");
    copy_str(st->ck.path, sizeof(st->ck.path), st->path, NULL);
    st->ck.flags = 0;
    if (env->sysinfo(RV9_SYS_CHECK, &st->ck, sizeof(st->ck)) == 1) {
        char hex[65];
        for (int i = 0; i < 32; i++) {
            hex[2 * i] = "0123456789abcdef"[st->ck.sha256[i] >> 4];
            hex[2 * i + 1] = "0123456789abcdef"[st->ck.sha256[i] & 15];
        }
        hex[64] = '\0';
        put(env, c, hex);
    } else {
        put(env, c, "-");
    }
    put(env, c, "\n");
}

static void catalog(const rv9_mod_env_t *env, httpd_statics_t *st, int c)
{
    int d = env->open("/m0", RV9_MODE_READ);
    if (d < 0) { not_found(env, c); return; }
    status(env, c, "200 OK", -1);           /* the length is where it ends */
    for (;;) {
        int got = env->read(d, st->ents, sizeof(st->ents));
        if (got <= 0) break;
        for (int i = 0; i < got / (int)sizeof(rv9_dirent_t); i++) {
            catalog_line(env, st, c, &st->ents[i]);
        }
    }
    env->close(d);
    st->served++;
}

/* Does any module /m0 holds declare that it reads `file`? */
static bool declared(const rv9_mod_env_t *env, httpd_statics_t *st, const char *file)
{
    uint32_t want = m_len(file);
    int d = env->open("/m0", RV9_MODE_READ);
    if (d < 0) return false;
    bool found = false;
    while (!found) {
        int got = env->read(d, st->ents, sizeof(st->ents));
        if (got <= 0) break;
        for (int i = 0; i < got / (int)sizeof(rv9_dirent_t) && !found; i++) {
            copy_str((char *)&st->n, 29, st->ents[i].name, NULL);
            if (env->sysinfo(RV9_SYS_NEEDS, &st->n, sizeof(st->n) + sizeof(st->more)) != 1) {
                continue;
            }
            const uint8_t *m = st->n.manifest;
            for (uint32_t off = 0; off + 4 <= st->n.len; ) {
                uint16_t tag = (uint16_t)(m[off] | (m[off + 1] << 8));
                uint16_t len = (uint16_t)(m[off + 2] | (m[off + 3] << 8));
                if (RV9_MTAG_NUMBER(tag) == RV9_MTAG_END || off + 4 + len > st->n.len) break;
                /* "/sd0/whistle.cact", or with its hash after an '=' */
                if (RV9_MTAG_NUMBER(tag) == RV9_MTAG_FILE &&
                    (len == want || (len > want && m[off + 4 + want] == '='))) {
                    bool same = true;
                    for (uint32_t k = 0; k < want && same; k++) same = m[off + 4 + k] == (uint8_t)file[k];
                    if (same) found = true;
                }
                uint32_t next = off + 4 + len;
                off = next + (4 - (next % 4)) % 4;
            }
        }
    }
    env->close(d);
    return found;
}

static void serve(const rv9_mod_env_t *env, httpd_statics_t *st, int c)
{
    /* The request line and headers, up to the blank line or a full buffer. */
    uint32_t n = 0;
    while (n < sizeof(st->req) - 1) {
        int got = env->read(c, st->req + n, sizeof(st->req) - 1 - n);
        if (got <= 0) break;
        n += (uint32_t)got;
        st->req[n] = '\0';
        bool end = false;
        for (uint32_t i = 3; i < n && !end; i++) {
            end = st->req[i] == '\n' && (st->req[i - 1] == '\n' ||
                  (st->req[i - 1] == '\r' && st->req[i - 2] == '\n'));
        }
        if (end) break;
    }
    st->req[n] = '\0';
    if (!starts(st->req, "GET /")) {
        status(env, c, "400 Bad Request", 0);
        return;
    }

    char *url = st->req + 4;
    uint32_t i = 0;
    while (url[i] && url[i] != ' ' && url[i] != '\r' && url[i] != '\n') i++;
    url[i] = '\0';

    if (m_eq(url, "/catalog")) {
        catalog(env, st, c);
    } else if (starts(url, "/cmd/")) {
        char *name = url + 5;
        uint32_t len = m_len(name);
        if (len > 4 && m_eq(name + len - 4, ".mod")) {
            name[len - 4] = '\0';
            if (plain_name(name)) {
                copy_str(st->path, sizeof(st->path), "/m0/", name);
                send_file(env, st, c, st->path);
                return;
            }
        }
        not_found(env, c);
    } else if (starts(url, "/files/") && plain_name(url + 7)) {
        copy_str(st->path, sizeof(st->path), "/sd0/", url + 7);
        if (declared(env, st, st->path)) {
            send_file(env, st, c, st->path);
        } else {
            not_found(env, c);
        }
    } else {
        not_found(env, c);
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 17) return 2;   /* RV9_SYS_CHECK */

    httpd_statics_t *st = (httpd_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    uint32_t port = DEFAULT_PORT;
    int f = env->open(SERVE_FILE, RV9_MODE_READ);
    if (f >= 0) {
        char num[12];
        int got = env->read(f, num, sizeof(num) - 1);
        env->close(f);
        if (got > 0) {
            num[got] = '\0';
            uint32_t p = m_num_parse(num, NULL);
            if (p > 0 && p < 65536) port = p;
        }
    }
    copy_str(st->listen, sizeof(st->listen), "/n0/listen/", NULL);
    uint32_t at = m_len(st->listen);
    char digits[6];
    uint32_t nd = 0;
    do { digits[nd++] = (char)('0' + port % 10); port /= 10; } while (port);
    while (nd) st->listen[at++] = digits[--nd];
    st->listen[at] = '\0';

    for (;;) {
        /* As rshd: an open that fails at once must not become a spin. */
        int c = env->open(st->listen, RV9_MODE_RW);
        if (c < 0) {
            env->sleep_ms(1000);
            continue;
        }
        serve(env, st, c);
        env->close(c);
    }
    return 0;
}
