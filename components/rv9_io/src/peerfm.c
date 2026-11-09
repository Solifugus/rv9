/*
 * PEERFM -- another machine's modules, as files.
 *
 *     /peer                  the peers /f0/peers names, as a directory
 *     /peer/<name>/<rest>    GET /<rest> from that peer
 *
 * so that /peer/p4/catalog is what the P4's httpd serves at /catalog, and
 * /peer/p4/cmd/listen.mod is its listen. A module path entry of
 * /peer/p4/cmd/ is then searched as any other (design §74): the module
 * manager opens <dir><name>.mod, and whether that is the card or another
 * board is this file's business, not its.
 *
 * /f0/peers, one peer a line, read at every open as /f0/path is:
 *
 *     p4  192.168.1.178:8080
 *
 * Read-only. A file is one GET over /n0, which this stacks on as ssh does,
 * reached by a detached path. Its length is the answer's Content-Length;
 * a seek forward reads and drops, and a seek back asks again, which is
 * what reading a module's header and then the whole of it costs.
 *
 * WHAT IT DOES NOT TRUST
 *
 * The peer. A module read from here must be signed by a key in /f0/trust
 * even on a machine with none (trust_check in module.c) -- it is the one
 * place a module can come from that nobody at this machine put there.
 *
 * WHEN THE PEER IS NOT THERE
 *
 * Every name the shell does not know is looked for along the path, so a
 * peer that is off must not cost a wait each time. One that cannot be
 * reached in CONNECT_MS is passed over for PEER_DOWN_MS; one that is busy --
 * httpd takes a connection at a time -- is asked again, a few times.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "rv9/net.h"

static const char *TAG = "rv9-peer";

#define PEERS_FILE    "/f0/peers"
#define PEERS_MAX_LEN 512
#define PEER_NAME_MAX 16
#define HEAD_MAX      512          /* status line and headers */
#define READ_WAIT_MS  10000        /* nothing at all for this long: gone */
#define POLL_MS       10
#define TRIES         3
#define RETRY_MS      300
#define PEER_DOWN_MS  60000
#define CONNECT_MS    2000         /* a board on this network answers in ms */
#define DOWN_SLOTS    8

typedef struct {
    bool       is_dir;
    rv9_path_t *conn;
    char       peer[PEER_NAME_MAX];
    char       endpoint[48];       /* "/n0/<host>/<port>/<connect ms>" */
    char       url[80];            /* "/<rest>" */
    int64_t    size;               /* Content-Length, or -1 */
    int64_t    at;                 /* body bytes taken from the connection */
    uint32_t   held, used;         /* body bytes that came with the headers */
    char       head[HEAD_MAX];
} peer_path_t;

/* Peers that did not answer, and until when they are not asked. */
static struct { char name[PEER_NAME_MAX]; uint64_t until; } s_down[DOWN_SLOTS];

static bool is_down(const char *peer)
{
    uint64_t now = rv9_time_ms();
    for (int i = 0; i < DOWN_SLOTS; i++) {
        if (strcmp(s_down[i].name, peer) == 0) return now < s_down[i].until;
    }
    return false;
}

static void mark(const char *peer, bool down)
{
    int slot = -1;
    for (int i = 0; i < DOWN_SLOTS; i++) {
        if (strcmp(s_down[i].name, peer) == 0) { slot = i; break; }
        if (slot < 0 && rv9_time_ms() >= s_down[i].until) slot = i;
    }
    if (slot < 0) return;
    if (!down) {
        if (strcmp(s_down[slot].name, peer) == 0) s_down[slot].until = 0;
        return;
    }
    strncpy(s_down[slot].name, peer, PEER_NAME_MAX - 1);
    s_down[slot].until = rv9_time_ms() + PEER_DOWN_MS;
}

static int read_peers(char *buf, uint32_t cap)
{
    rv9_path_t *p = NULL;
    if (rv9_io_open_detached(PEERS_FILE, RV9_MODE_READ, &p) != RV9_IO_OK) return -1;
    uint32_t got = 0;
    size_t n = 0;
    while (got < cap && rv9_io_read_path(p, buf + got, cap - got, &n) == RV9_IO_OK && n > 0) {
        got += (uint32_t)n;
    }
    rv9_io_close_path(p);
    buf[got] = '\0';
    return (int)got;
}

/*
 * Each line of /f0/peers, split: name and host[:port]. Calls `each` until
 * it returns true. A line starting with # is a comment.
 */
typedef bool (*peer_fn)(const char *name, const char *addr, void *arg);

static bool each_peer(char *text, peer_fn each, void *arg)
{
    for (char *line = text; *line; ) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char save = *end;
        *end = '\0';

        char *f[2] = { NULL, NULL };
        char *c = line;
        for (int k = 0; k < 2; k++) {
            while (*c == ' ' || *c == '\t' || *c == '\r') c++;
            if (*c == '\0') break;
            f[k] = c;
            while (*c && *c != ' ' && *c != '\t' && *c != '\r') c++;
            if (*c) *c++ = '\0';
        }
        bool stop = f[1] != NULL && f[0][0] != '#' && strlen(f[0]) < PEER_NAME_MAX &&
                    each(f[0], f[1], arg);
        *end = save;
        if (stop) return true;
        line = save ? end + 1 : end;
    }
    return false;
}

static bool find_one(const char *name, const char *addr, void *arg)
{
    peer_path_t *st = (peer_path_t *)arg;
    if (strcmp(name, st->peer) != 0) return false;

    const char *colon = strchr(addr, ':');
    size_t hl = colon ? (size_t)(colon - addr) : strlen(addr);
    const char *port = colon ? colon + 1 : "8080";
    int n = snprintf(st->endpoint, sizeof(st->endpoint), "/n0/%.*s/%s/%d",
                     (int)hl, addr, port, CONNECT_MS);
    return hl > 0 && n > 0 && n < (int)sizeof(st->endpoint);
}

static void hang_up(peer_path_t *st)
{
    if (st->conn) rv9_io_close_path(st->conn);
    st->conn = NULL;
}

/* Whatever the connection gives next, waiting at most READ_WAIT_MS. */
static rv9_io_err_t recv_some(peer_path_t *st, void *buf, size_t len, size_t *done)
{
    for (uint32_t waited = 0; ; waited += POLL_MS) {
        rv9_io_err_t e = rv9_io_read_path(st->conn, buf, len, done);
        if (e != RV9_IO_ERR_WOULDBLOCK) return e;
        if (waited >= READ_WAIT_MS || rv9_task_cancelled()) {
            ESP_LOGW(TAG, "%s: no answer in %d ms", st->peer, READ_WAIT_MS);
            return RV9_IO_ERR_TIMEOUT;
        }
        rv9_task_delay_ms(POLL_MS);
    }
}

/* One GET: connect, ask, and read the answer up to its body. */
static rv9_io_err_t ask_once(peer_path_t *st)
{
    rv9_io_err_t e = rv9_io_open_detached(st->endpoint, RV9_MODE_RW, &st->conn);
    if (e != RV9_IO_OK) { st->conn = NULL; return e; }
    uint32_t one = 1;
    rv9_io_setstat_path(st->conn, RV9_NET_SS_NOWAIT, &one);

    /* The request is short enough to go in the buffer the answer will use. */
    char *req = st->head;
    int n = snprintf(req, HEAD_MAX, "GET %s HTTP/1.0\r\nUser-Agent: rv9-peer\r\n\r\n", st->url);
    size_t done = 0;
    if (rv9_io_write_path(st->conn, req, (size_t)n, &done) != RV9_IO_OK || done != (size_t)n) {
        hang_up(st);
        return RV9_IO_ERR_IO;
    }

    uint32_t got = 0;
    char *body = NULL;
    while (body == NULL) {
        if (got >= HEAD_MAX - 1) { hang_up(st); return RV9_IO_ERR_IO; }
        e = recv_some(st, st->head + got, HEAD_MAX - 1 - got, &done);
        if (e != RV9_IO_OK) { hang_up(st); return e; }
        if (done == 0) { hang_up(st); return RV9_IO_ERR_IO; }    /* closed early */
        got += (uint32_t)done;
        st->head[got] = '\0';
        char *b = strstr(st->head, "\r\n\r\n");
        if (b) body = b + 4;
        else if ((b = strstr(st->head, "\n\n")) != NULL) body = b + 2;
    }

    /* "HTTP/1.0 200 OK" */
    const char *sp = strchr(st->head, ' ');
    int code = sp ? atoi(sp + 1) : 0;
    if (code != 200) {
        hang_up(st);
        return code == 404 ? RV9_IO_ERR_NOTFOUND : RV9_IO_ERR_IO;
    }

    st->size = -1;
    for (char *h = strchr(st->head, '\n'); h && h < body; h = strchr(h, '\n')) {
        h++;
        if (strncasecmp(h, "Content-Length:", 15) == 0) st->size = atoll(h + 15);
    }
    st->held = (uint32_t)(st->head + got - body);
    memmove(st->head, body, st->held);
    st->used = 0;
    st->at = 0;
    return RV9_IO_OK;
}

/* Ask, again if the peer is busy; remember a peer that is not there. */
static rv9_io_err_t ask(peer_path_t *st)
{
    if (is_down(st->peer)) return RV9_IO_ERR_NOTFOUND;
    rv9_io_err_t e = RV9_IO_ERR_IO;
    for (int t = 0; t < TRIES; t++) {
        if (t) rv9_task_delay_ms(RETRY_MS);
        e = ask_once(st);
        if (e == RV9_IO_OK || e == RV9_IO_ERR_NOTFOUND) {
            mark(st->peer, false);
            return e;
        }
        if (e == RV9_IO_ERR_TIMEOUT) break;      /* not busy: not there */
    }
    ESP_LOGW(TAG, "%s (%s): %s; not asked again for %d s", st->peer, st->endpoint,
             rv9_io_strerror(e), PEER_DOWN_MS / 1000);
    mark(st->peer, true);
    return e;
}

static rv9_io_err_t peer_open(rv9_path_t *path, const char *rest)
{
    if (path->mode & (RV9_MODE_WRITE | RV9_MODE_CREATE)) return RV9_IO_ERR_MODE;

    peer_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;

    if (rest == NULL || rest[0] == '\0') {
        st->is_dir = true;
        path->fm_state = st;
        return RV9_IO_OK;
    }

    /* "p4/cmd/listen.mod": the peer, then what to ask it for. */
    const char *slash = strchr(rest, '/');
    size_t pl = slash ? (size_t)(slash - rest) : strlen(rest);
    if (pl == 0 || pl >= PEER_NAME_MAX || slash == NULL || slash[1] == '\0' ||
        strlen(slash) >= sizeof(st->url)) {
        rv9_free(st);
        return RV9_IO_ERR_NOTFOUND;
    }
    memcpy(st->peer, rest, pl);
    strcpy(st->url, slash);

    char *peers = rv9_alloc(PEERS_MAX_LEN + 1);
    if (peers == NULL) { rv9_free(st); return RV9_IO_ERR_NOMEM; }
    bool known = read_peers(peers, PEERS_MAX_LEN) > 0 && each_peer(peers, find_one, st);
    rv9_free(peers);
    if (!known) { rv9_free(st); return RV9_IO_ERR_NOTFOUND; }

    rv9_io_err_t e = ask(st);
    if (e != RV9_IO_OK) { rv9_free(st); return e; }
    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t peer_close(rv9_path_t *path)
{
    /* Called with the I/O manager's lock held, so not hang_up. */
    peer_path_t *st = (peer_path_t *)path->fm_state;
    if (st && st->conn) rv9_io_close_below(st->conn);
    rv9_free(st);
    path->fm_state = NULL;
    return RV9_IO_OK;
}

/* The directory: one entry per peer, `pos` counting them. */
typedef struct { rv9_dirent_t *out; size_t room, n; int64_t skip; } list_t;

static bool list_one(const char *name, const char *addr, void *arg)
{
    (void)addr;
    list_t *l = (list_t *)arg;
    if (l->skip > 0) { l->skip--; return false; }
    rv9_dirent_t *d = l->out + l->n++;
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    return l->n >= l->room;
}

static rv9_io_err_t dir_read(rv9_path_t *path, void *buf, size_t len, size_t *done)
{
    list_t l = { .out = buf, .room = len / sizeof(rv9_dirent_t), .skip = path->pos };
    *done = 0;
    if (l.room == 0) return RV9_IO_OK;
    char *peers = rv9_alloc(PEERS_MAX_LEN + 1);
    if (peers == NULL) return RV9_IO_ERR_NOMEM;
    if (read_peers(peers, PEERS_MAX_LEN) > 0) each_peer(peers, list_one, &l);
    rv9_free(peers);
    path->pos += (int64_t)l.n;
    *done = l.n * sizeof(rv9_dirent_t);
    return RV9_IO_OK;
}

/* The body: what came with the headers first, then the connection. */
static rv9_io_err_t body_read(peer_path_t *st, void *buf, size_t len, size_t *done)
{
    *done = 0;
    if (st->size >= 0 && st->at >= st->size) return RV9_IO_OK;
    if (st->size >= 0 && (int64_t)len > st->size - st->at) len = (size_t)(st->size - st->at);
    if (st->used < st->held) {
        size_t n = st->held - st->used;
        if (n > len) n = len;
        memcpy(buf, st->head + st->used, n);
        st->used += (uint32_t)n;
        st->at += (int64_t)n;
        *done = n;
        return RV9_IO_OK;
    }
    if (st->conn == NULL) return RV9_IO_ERR_IO;
    rv9_io_err_t e = recv_some(st, buf, len, done);
    if (e == RV9_IO_OK) st->at += (int64_t)*done;
    /* Ended before its length: the file is not all here. */
    if (e == RV9_IO_OK && *done == 0 && st->size >= 0 && st->at < st->size) e = RV9_IO_ERR_IO;
    return e;
}

static rv9_io_err_t peer_read(rv9_path_t *path, void *buf, size_t len, size_t *done)
{
    peer_path_t *st = (peer_path_t *)path->fm_state;
    if (st == NULL || done == NULL) return RV9_IO_ERR_INVAL;
    if (st->is_dir) return dir_read(path, buf, len, done);
    rv9_io_err_t e = body_read(st, buf, len, done);
    if (e == RV9_IO_OK) path->pos = st->at;
    return e;
}

static rv9_io_err_t peer_write(rv9_path_t *path, const void *buf, size_t len, size_t *done)
{
    (void)path; (void)buf; (void)len; (void)done;
    return RV9_IO_ERR_MODE;
}

static rv9_io_err_t peer_seek(rv9_path_t *path, int64_t offset, int whence)
{
    peer_path_t *st = (peer_path_t *)path->fm_state;
    if (st == NULL || st->is_dir) return RV9_IO_ERR_UNSUPPORTED;
    int64_t base = whence == RV9_SEEK_SET ? 0 : whence == RV9_SEEK_CUR ? st->at : st->size;
    int64_t to = base + offset;
    if (base < 0 || to < 0 || (st->size >= 0 && to > st->size)) return RV9_IO_ERR_INVAL;

    if (to < st->at) {
        /* Back: within what came with the headers, if nothing past it has
           been read yet; else ask again. */
        if (st->at <= (int64_t)st->held) {
            st->used = (uint32_t)to;
            st->at = to;
        } else {
            hang_up(st);
            rv9_io_err_t e = ask(st);
            if (e != RV9_IO_OK) return e;
        }
    }
    uint8_t drop[64];
    while (st->at < to) {
        size_t want = (size_t)(to - st->at), n = 0;
        if (want > sizeof(drop)) want = sizeof(drop);
        rv9_io_err_t e = body_read(st, drop, want, &n);
        if (e != RV9_IO_OK) return e;
        if (n == 0) return RV9_IO_ERR_INVAL;
    }
    path->pos = st->at;
    return RV9_IO_OK;
}

static rv9_io_err_t peer_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    peer_path_t *st = (peer_path_t *)path->fm_state;
    if (code == RV9_GS_SIZE && arg && st && !st->is_dir && st->size >= 0) {
        *(uint64_t *)arg = (uint64_t)st->size;
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t peer_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    (void)path; (void)code; (void)arg;
    return RV9_IO_ERR_UNSUPPORTED;
}

static const rv9_filemgr_t peerfm = {
    .name    = "peerfm",
    .open    = peer_open,
    .close   = peer_close,
    .read    = peer_read,
    .write   = peer_write,
    .seek    = peer_seek,
    .getstat = peer_getstat,
    .setstat = peer_setstat,
};

rv9_io_err_t rv9_peerfm_register(void)
{
    return rv9_io_register_filemgr(&peerfm);
}

/* peer: the driver under /peer. The network is /n0's; this has none. */
static rv9_io_err_t peer_init(rv9_dev_t *dev)
{
    (void)dev;
    return RV9_IO_OK;
}

static const rv9_driver_t peer_drv = {
    .name = "peer",
    .init = peer_init,
};

rv9_io_err_t rv9_drv_peer_register(void)
{
    return rv9_io_register_driver(&peer_drv);
}
