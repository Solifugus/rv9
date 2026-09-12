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
#include "rv9/net.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <fcntl.h>

static const char *TAG = "rv9-nfm";

#define CONNECT_TIMEOUT_MS 15000
#define ACCEPT_BACKLOG     1
#define POLL_MS            10

/*
 * Accepting and receiving wait indefinitely, the way reading a terminal
 * does. A path that is not ready yet blocks the reader; that is what
 * reading means, and a caller who wants otherwise can ask whether data is
 * ready first.
 *
 * They used to give up after fifteen seconds, which made a listening
 * daemon listen only *most* of the time -- connections arriving in the gap
 * were refused -- and quietly ended any session idle for a quarter of a
 * minute.
 *
 * Connecting still gives up: an unreachable host should be reported, not
 * waited on forever.
 */

/*
 * Every socket here is non-blocking, and waiting is done by sleeping
 * through the scheduler rather than inside lwIP.
 *
 * Blocking in a socket call parks whichever thread made it -- which is
 * fine when each thread is a host task, and fatal when RV-9's own kernel
 * is running all of its threads inside one. A listener sitting in accept()
 * stopped the entire operating system, including the process that was
 * about to connect to it.
 *
 * Polling costs a little latency and makes the driver correct under both
 * kernels, which is the trade a driver should make: it has no business
 * knowing how many host tasks its callers are sharing.
 */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool would_block(void)
{
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}

typedef struct {
    int  fd;
    bool is_device;     /* "/n0" itself, with no endpoint behind it */
    bool nowait;        /* RV9_NET_SS_NOWAIT: do not wait for data */
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
    set_nonblocking(fd);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 &&
        !would_block()) {
        ESP_LOGW(TAG, "connect to %s:%u failed: errno %d (%s)",
                 host, (unsigned)port, errno, strerror(errno));
        close(fd);
        return RV9_IO_ERR_IO;
    }

    /* Wait for the handshake by asking whether the socket has become
       writable, sleeping in between so other threads run. */
    for (uint32_t waited = 0; waited < CONNECT_TIMEOUT_MS; waited += POLL_MS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval zero = { 0, 0 };

        if (select(fd + 1, NULL, &wfds, NULL, &zero) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                *out_fd = fd;
                return RV9_IO_OK;
            }
            ESP_LOGW(TAG, "connect to %s:%u refused: errno %d",
                     host, (unsigned)port, err);
            close(fd);
            return RV9_IO_ERR_IO;
        }
        rv9_task_delay_ms(POLL_MS);
    }

    ESP_LOGW(TAG, "connect to %s:%u timed out", host, (unsigned)port);
    close(fd);
    return RV9_IO_ERR_TIMEOUT;
}

static rv9_io_err_t accept_in(uint16_t port, int *out_fd)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return RV9_IO_ERR_NOMEM;

    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblocking(listener);

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

    ESP_LOGI(TAG, "listening on %u", (unsigned)port);

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = -1;

    for (;;) {
        peer_len = sizeof(peer);
        fd = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (fd >= 0) break;
        if (!would_block()) {
            ESP_LOGW(TAG, "accept on %u failed: errno %d (%s)",
                     (unsigned)port, errno, strerror(errno));
            break;
        }
        rv9_task_delay_ms(POLL_MS);
    }

    /* The listener has done its job; only the connection is a path. */
    close(listener);

    if (fd < 0) return RV9_IO_ERR_TIMEOUT;

    set_nonblocking(fd);
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

    for (;;) {
        int n = recv(st->fd, buf, len, 0);
        if (n >= 0) {
            if (done) *done = (size_t)n;
            return RV9_IO_OK;   /* n == 0 means the peer closed */
        }
        if (!would_block()) break;
        if (st->nowait) return RV9_IO_ERR_WOULDBLOCK;
        rv9_task_delay_ms(POLL_MS);
    }

    if (done) *done = 0;
    return RV9_IO_ERR_IO;
}

static rv9_io_err_t nfm_write(rv9_path_t *path, const void *buf, size_t len,
                              size_t *done)
{
    nfm_path_t *st = (nfm_path_t *)path->fm_state;
    if (st == NULL || st->fd < 0) return RV9_IO_ERR_IO;

    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    uint32_t waited = 0;
    while (sent < len && waited < CONNECT_TIMEOUT_MS) {
        int n = send(st->fd, p + sent, len - sent, 0);
        if (n > 0) { sent += (size_t)n; waited = 0; continue; }
        if (n < 0 && !would_block()) break;
        rv9_task_delay_ms(POLL_MS);
        waited += POLL_MS;
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

    /*
     * A connection has no way to know how big the terminal at the far end
     * is -- nothing in a TCP stream says so. The guess is stated in io.h
     * and is the same one every serial line makes.
     */
    if (code == RV9_CON_GS_SIZE && arg) {
        *(uint32_t *)arg = (RV9_CON_DEFAULT_ROWS << 16) | RV9_CON_DEFAULT_COLS;
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
    if (code == RV9_NET_SS_NOWAIT) {
        nfm_path_t *st = (nfm_path_t *)path->fm_state;
        if (st == NULL || arg == NULL) return RV9_IO_ERR_INVAL;
        st->nowait = (*(uint32_t *)arg != 0);
        return RV9_IO_OK;
    }

    /*
     * The far end of a socket is somebody's terminal, so it gets the same
     * escape sequences a serial line does. The driver is never offered
     * these: a network card has no cursor, and asking it would only be
     * ceremony.
     */
    if (arg != NULL) {
        char seq[RV9_CON_ANSI_MAX];
        size_t n = rv9_con_ansi(seq, sizeof(seq), code, *(uint32_t *)arg);
        if (n > 0) {
            size_t moved = 0;
            return nfm_write(path, seq, n, &moved);
        }
    }

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
