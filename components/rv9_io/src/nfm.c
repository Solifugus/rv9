/*
 * NFM -- network file manager.
 *
 * A network endpoint is a path. You open it, read it, write it and close
 * it, and there are no network-specific system calls anywhere above this
 * file. That is the claim the I/O design has been making since phase 3,
 * and a socket is the third shape it has had to survive -- after a
 * character stream and a block device -- so it is the one that tests it.
 *
 * Path forms:
 *
 *   /n0                     the device itself: status and configuration
 *   /n0/<host>/<port>       connect outbound, e.g. /n0/127.0.0.1/8080
 *   /n0/listen/<port>       wait for one inbound connection
 *
 * Opening a listener blocks until someone connects, which is the same
 * shape as opening any other path that is not ready yet. The alternative
 * -- accept() as a separate call -- would be a network-specific verb, and
 * the whole point is not to have any.
 *
 * lwIP is used through BSD sockets. In a stricter arrangement the driver
 * would own the interface and NFM would talk to it, but lwIP already spans
 * both jobs, so the driver below handles the link and NFM handles the
 * endpoints.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "rv9-nfm";

#define RECV_TIMEOUT_MS  15000
#define ACCEPT_BACKLOG   1

typedef struct {
    int  fd;
    bool is_device;     /* "/n0" itself, with no endpoint behind it */
} nfm_path_t;

/* Split "listen/8080" or "127.0.0.1/8080" at the last slash. */
static bool split_endpoint(const char *rest, char *host, size_t host_len,
                           uint16_t *port)
{
    const char *slash = strrchr(rest, '/');
    if (slash == NULL || slash == rest) return false;

    size_t n = (size_t)(slash - rest);
    if (n >= host_len) return false;

    memcpy(host, rest, n);
    host[n] = '\0';

    int p = atoi(slash + 1);
    if (p <= 0 || p > 65535) return false;

    *port = (uint16_t)p;
    return true;
}

static rv9_io_err_t connect_out(const char *host, uint16_t port, int *out_fd)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        /* Not a dotted quad; try a name. */
        struct hostent *he = gethostbyname(host);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            ESP_LOGW(TAG, "cannot resolve '%s'", host);
            return RV9_IO_ERR_NOTFOUND;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return RV9_IO_ERR_NOMEM;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "connect to %s:%u failed: errno %d (%s)",
                 host, (unsigned)port, errno, strerror(errno));
        close(fd);
        return RV9_IO_ERR_IO;
    }

    struct timeval tv = { .tv_sec = RECV_TIMEOUT_MS / 1000, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    *out_fd = fd;
    return RV9_IO_OK;
}

static rv9_io_err_t accept_in(uint16_t port, int *out_fd)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return RV9_IO_ERR_NOMEM;

    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, ACCEPT_BACKLOG) != 0) {
        ESP_LOGW(TAG, "cannot listen on %u: errno %d",
                 (unsigned)port, errno);
        close(listener);
        return RV9_IO_ERR_IO;
    }

    struct timeval tv = { .tv_sec = RECV_TIMEOUT_MS / 1000, .tv_usec = 0 };
    setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "listening on %u", (unsigned)port);

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept(listener, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        ESP_LOGW(TAG, "accept on %u failed: errno %d (%s)",
                 (unsigned)port, errno, strerror(errno));
    }

    /* The listener has done its job; only the connection is a path. */
    close(listener);

    if (fd < 0) return RV9_IO_ERR_TIMEOUT;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    *out_fd = fd;
    return RV9_IO_OK;
}

/* ------------------------------------------------------------------ */
/* File manager entry points                                           */
/* ------------------------------------------------------------------ */

static rv9_io_err_t nfm_open(rv9_path_t *path, const char *rest)
{
    nfm_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;
    st->fd = -1;

    if (rest == NULL || rest[0] == '\0') {
        st->is_device = true;
        path->fm_state = st;
        return RV9_IO_OK;
    }

    char host[64];
    uint16_t port = 0;
    if (!split_endpoint(rest, host, sizeof(host), &port)) {
        rv9_free(st);
        return RV9_IO_ERR_INVAL;
    }

    rv9_io_err_t err;
    if (strcmp(host, "listen") == 0) {
        err = accept_in(port, &st->fd);
    } else {
        ESP_LOGI(TAG, "connecting to %s:%u", host, (unsigned)port);
        err = connect_out(host, port, &st->fd);
    }

    if (err != RV9_IO_OK) {
        rv9_free(st);
        return err;
    }

    strncpy(path->name, rest, sizeof(path->name) - 1);
    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t nfm_close(rv9_path_t *path)
{
    nfm_path_t *st = (nfm_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_OK;

    if (st->fd >= 0) close(st->fd);
    rv9_free(st);
    path->fm_state = NULL;
    return RV9_IO_OK;
}

static rv9_io_err_t nfm_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    nfm_path_t *st = (nfm_path_t *)path->fm_state;
    if (st == NULL || st->fd < 0) return RV9_IO_ERR_IO;

    int n = recv(st->fd, buf, len, 0);
    if (n < 0) { if (done) *done = 0; return RV9_IO_ERR_TIMEOUT; }

    if (done) *done = (size_t)n;
    return RV9_IO_OK;     /* n == 0 means the peer closed; a short read */
}

static rv9_io_err_t nfm_write(rv9_path_t *path, const void *buf, size_t len,
                              size_t *done)
{
    nfm_path_t *st = (nfm_path_t *)path->fm_state;
    if (st == NULL || st->fd < 0) return RV9_IO_ERR_IO;

    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        int n = send(st->fd, p + sent, len - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }

    if (done) *done = sent;
    return (sent == len) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

/* A stream has no position, and saying so is more useful than pretending. */
static rv9_io_err_t nfm_seek(rv9_path_t *path, int64_t offset, int whence)
{
    (void)path; (void)offset; (void)whence;
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t nfm_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    nfm_path_t *st = (nfm_path_t *)path->fm_state;

    if (code == RV9_GS_READY && arg && st && st->fd >= 0) {
        int avail = 0;
        if (ioctl(st->fd, FIONREAD, &avail) != 0) return RV9_IO_ERR_IO;
        *(uint32_t *)arg = (uint32_t)avail;
        return RV9_IO_OK;
    }

    /* Anything else belongs to the driver -- link status, address. */
    if (path->dev->drv->getstat) {
        return path->dev->drv->getstat(path->dev, code, arg);
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t nfm_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    if (path->dev->drv->setstat) {
        return path->dev->drv->setstat(path->dev, code, arg);
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static const rv9_filemgr_t nfm = {
    .name    = "nfm",
    .open    = nfm_open,
    .close   = nfm_close,
    .read    = nfm_read,
    .write   = nfm_write,
    .seek    = nfm_seek,
    .getstat = nfm_getstat,
    .setstat = nfm_setstat,
};

rv9_io_err_t rv9_nfm_register(void)
{
    return rv9_io_register_filemgr(&nfm);
}
