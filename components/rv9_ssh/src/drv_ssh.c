/*
 * ssh -- a character driver whose device is an encrypted session.
 *
 * The shape is the point. An SSH session is a stream of bytes with a
 * terminal at the far end, which is what a character driver is for, so it
 * is one -- and everything SCF already does for a UART it now does for
 * this: line discipline, echo, rubout, ^C, newline translation, and the
 * console setstats that put a cursor where you asked.
 *
 * That last one matters more than it sounds. An ssh client that has been
 * given a pty puts its own terminal in raw mode and echoes nothing: every
 * keystroke arrives here and the far end shows only what we send back. A
 * session without a line discipline would be a session where typing
 * appears to do nothing. SCF was written for a serial port in phase 3 and
 * turns out to be exactly what this needs.
 *
 * Underneath, the connection is a path -- `/n0/listen/22` -- opened and
 * held by the driver. There is no socket in this file, and no lwIP.
 */
#include "ssh.h"

#include "rv9/kal.h"
#include "rv9/sshd.h"
#include "rv9/ssh_builtin.h"

#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "rv9-ssh";

/* Descriptor options, by index into rv9_devdesc_t.opt.
   0 and 1 are SCF's echo and autolf, which is why these start at 2. */
#define OPT_PORT   2
#define OPT_CONFIG 3

#define AUTH_TRIES 6

/* ------------------------------------------------------------------ */
/* Sending the small messages                                          */
/* ------------------------------------------------------------------ */

static rv9_io_err_t send_u8(ssh_t *s, uint8_t msg)
{
    ssh_buf_t p;
    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, msg);
    return ssh_packet_send(s, &p);
}

static rv9_io_err_t send_chan_reply(ssh_t *s, uint8_t msg)
{
    ssh_buf_t p;
    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, msg);
    ssh_put_u32(&p, s->peer_chan);
    return ssh_packet_send(s, &p);
}

/*
 * Give the client back the credit it spent sending us data.
 *
 * Topped up in one go when half the window is gone, rather than after
 * every packet: an adjustment per keystroke would double the traffic of an
 * interactive session to no purpose.
 */
static void window_top_up(ssh_t *s, uint32_t used)
{
    s->my_window = (s->my_window > used) ? s->my_window - used : 0;
    if (s->my_window > SSH_WINDOW_INIT / 2) return;

    uint32_t add = SSH_WINDOW_INIT - s->my_window;

    ssh_buf_t p;
    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    ssh_put_u32(&p, s->peer_chan);
    ssh_put_u32(&p, add);

    if (ssh_packet_send(s, &p) == RV9_IO_OK) s->my_window += add;
}

/* ------------------------------------------------------------------ */
/* Authentication                                                      */
/* ------------------------------------------------------------------ */

static rv9_io_err_t auth_failure(ssh_t *s)
{
    ssh_buf_t p;
    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, SSH_MSG_USERAUTH_FAILURE);
    ssh_put_cstr(&p, "publickey,password");
    ssh_put_u8(&p, 0);          /* no partial success */
    return ssh_packet_send(s, &p);
}

/*
 * Public key authentication.
 *
 * Two independent things must hold: the key has to be one we trust, and
 * the client has to prove it holds the private half. Checking either
 * alone is how an SSH server comes to let anybody in -- the first without
 * the second accepts a key anyone can copy from a public repository, and
 * the second without the first accepts a valid signature from a stranger.
 *
 * The client asks twice. First without a signature, to find out whether
 * the key is worth the trouble of using -- answered with PK_OK, which is
 * not an authentication and grants nothing. Then with one.
 */
static rv9_io_err_t auth_publickey(ssh_t *s, ssh_buf_t *p, const char *user,
                                   const char *service, bool *ok, bool *probe)
{
    *ok = false;

    /*
     * An offer without a signature is a question, not an attempt, and must
     * not count against the try limit: a client with several keys asks
     * about each one in turn, and counting them would lock out anyone whose
     * agent holds a handful.
     */
    bool has_sig = ssh_get_u8(p) != 0;
    *probe = !has_sig;

    char alg[32];
    ssh_get_cstr(p, alg, sizeof(alg));

    size_t blob_len = 0;
    const uint8_t *blob = ssh_get_string(p, &blob_len);
    if (p->bad || blob == NULL) return RV9_IO_ERR_IO;

    if (!ssh_authkey_allowed(blob, blob_len)) {
        ESP_LOGW(TAG, "key offered by %s is not in " "/f0/authkeys", user);
        return RV9_IO_OK;      /* not an error, just not authorized */
    }

    if (!has_sig) {
        /* "That key would do. Sign with it." Grants nothing by itself. */
        ssh_buf_t r;
        ssh_packet_begin(s, &r);
        ssh_put_u8(&r, SSH_MSG_USERAUTH_PK_OK);
        ssh_put_cstr(&r, alg);
        ssh_put_string(&r, blob, blob_len);
        s->pk_ok = true;
        return ssh_packet_send(s, &r);
    }

    size_t sig_len = 0;
    const uint8_t *sig = ssh_get_string(p, &sig_len);
    if (p->bad || sig == NULL) return RV9_IO_ERR_IO;

    /*
     * Rebuild exactly what the client signed. The session id is in it,
     * which is what stops a signature captured from one session being
     * replayed into another -- the id is the first exchange hash, and no
     * two sessions share one.
     *
     * Built in the frame buffer: it belongs to the packet layer, which is
     * not in the middle of anything here, and a kilobyte of stack in a
     * process running a shell is not available.
     */
    ssh_buf_t d;
    ssh_buf_init(&d, s->frame, sizeof(s->frame));
    ssh_put_string(&d, s->session_id, SSH_HASH_LEN);
    ssh_put_u8(&d, SSH_MSG_USERAUTH_REQUEST);
    ssh_put_cstr(&d, user);
    ssh_put_cstr(&d, service);
    ssh_put_cstr(&d, "publickey");
    ssh_put_u8(&d, 1);
    ssh_put_cstr(&d, alg);
    ssh_put_string(&d, blob, blob_len);
    if (d.bad) return RV9_IO_ERR_INVAL;

    rv9_io_err_t err = ssh_pubkey_verify(blob, blob_len, alg,
                                         sig, sig_len, d.b, d.len);
    if (err == RV9_IO_OK) {
        *ok = true;
    } else {
        ESP_LOGW(TAG, "bad signature from %s using %s", user, alg);
    }
    return RV9_IO_OK;
}

/*
 * Password only, and one password for the whole board.
 *
 * There is no user database because there are no users: RV-9 has processes
 * and no notion of who owns one. Pretending otherwise -- accepting a name
 * and ignoring it -- would be worse than saying so, so the name is
 * recorded for the log and the password is what decides.
 *
 * Public key authentication is the better answer and is not here yet. It
 * needs somewhere to keep an authorized_keys, which wants the storage
 * phase, and signature verification, which the chip does in hardware.
 */
static rv9_io_err_t authenticate(ssh_t *s)
{
    int tries = 0;

    for (;;) {
        rv9_io_err_t err = ssh_packet_read(s);
        if (err != RV9_IO_OK) return err;
        if (s->pay_len == 0) return RV9_IO_ERR_IO;

        ssh_buf_t p;
        ssh_buf_load(&p, SSH_PAYLOAD(s), s->pay_len);
        uint8_t msg = ssh_get_u8(&p);

        if (msg == SSH_MSG_IGNORE || msg == SSH_MSG_DEBUG) continue;
        if (msg == SSH_MSG_DISCONNECT) return RV9_IO_ERR_IO;

        if (msg == SSH_MSG_SERVICE_REQUEST) {
            char name[32];
            ssh_get_cstr(&p, name, sizeof(name));
            if (p.bad || strcmp(name, "ssh-userauth") != 0) {
                ssh_disconnect(s, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE,
                               "only ssh-userauth");
                return RV9_IO_ERR_IO;
            }

            ssh_buf_t r;
            ssh_packet_begin(s, &r);
            ssh_put_u8(&r, SSH_MSG_SERVICE_ACCEPT);
            ssh_put_cstr(&r, "ssh-userauth");
            err = ssh_packet_send(s, &r);
            if (err != RV9_IO_OK) return err;
            continue;
        }

        if (msg != SSH_MSG_USERAUTH_REQUEST) continue;

        char user[32], service[32], method[32];
        ssh_get_cstr(&p, user, sizeof(user));
        ssh_get_cstr(&p, service, sizeof(service));
        ssh_get_cstr(&p, method, sizeof(method));
        if (p.bad) return RV9_IO_ERR_IO;

        if (strcmp(method, "publickey") == 0) {
            bool ok = false, probe = false;
            err = auth_publickey(s, &p, user, service, &ok, &probe);
            if (err != RV9_IO_OK) return err;

            if (ok) {
                strncpy(s->user, user, sizeof(s->user) - 1);
                ESP_LOGI(TAG, "%s logged in by key", s->user);
                return send_u8(s, SSH_MSG_USERAUTH_SUCCESS);
            }

            /* An accepted offer was already answered with PK_OK. */
            if (probe && s->pk_ok) { s->pk_ok = false; continue; }

            if (!probe && ++tries >= AUTH_TRIES) {
                ssh_disconnect(s, SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                               "too many attempts");
                return RV9_IO_ERR_MODE;
            }

            err = auth_failure(s);
            if (err != RV9_IO_OK) return err;
            continue;
        }

        if (strcmp(method, "password") == 0) {
            uint8_t change = ssh_get_u8(&p);
            char pass[65];
            ssh_get_cstr(&p, pass, sizeof(pass));

            bool ok = !p.bad && change == 0 && ssh_password_check(pass);
            memset(pass, 0, sizeof(pass));

            if (ok) {
                strncpy(s->user, user, sizeof(s->user) - 1);
                ESP_LOGI(TAG, "%s logged in", s->user);
                return send_u8(s, SSH_MSG_USERAUTH_SUCCESS);
            }
            ESP_LOGW(TAG, "failed password for %s", user);
        }

        /* "none" is how a client asks what is on offer, and is not a
           failed attempt -- counting it would lock out every client that
           asks politely before trying. */
        if (strcmp(method, "none") != 0 && ++tries >= AUTH_TRIES) {
            ssh_disconnect(s, SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                           "too many attempts");
            return RV9_IO_ERR_MODE;
        }

        err = auth_failure(s);
        if (err != RV9_IO_OK) return err;
    }
}

/* ------------------------------------------------------------------ */
/* The channel                                                         */
/* ------------------------------------------------------------------ */

static rv9_io_err_t channel_open(ssh_t *s, ssh_buf_t *p)
{
    char type[32];
    ssh_get_cstr(p, type, sizeof(type));
    uint32_t sender = ssh_get_u32(p);
    uint32_t window = ssh_get_u32(p);
    uint32_t maxpkt = ssh_get_u32(p);
    if (p->bad) return RV9_IO_ERR_IO;

    if (strcmp(type, "session") != 0 || s->chan_open) {
        ssh_buf_t r;
        ssh_packet_begin(s, &r);
        ssh_put_u8(&r, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_put_u32(&r, sender);
        ssh_put_u32(&r, 1);          /* administratively prohibited */
        ssh_put_cstr(&r, "one session channel only");
        ssh_put_cstr(&r, "");
        return ssh_packet_send(s, &r);
    }

    s->peer_chan   = sender;
    s->peer_window = window;
    s->peer_maxpkt = maxpkt ? maxpkt : SSH_MAX_PAYLOAD;
    s->my_window   = SSH_WINDOW_INIT;
    s->chan_open   = true;

    ssh_buf_t r;
    ssh_packet_begin(s, &r);
    ssh_put_u8(&r, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    ssh_put_u32(&r, sender);
    ssh_put_u32(&r, 0);                    /* our channel number */
    ssh_put_u32(&r, SSH_WINDOW_INIT);
    ssh_put_u32(&r, SSH_MAX_PAYLOAD);
    return ssh_packet_send(s, &r);
}

static rv9_io_err_t channel_request(ssh_t *s, ssh_buf_t *p)
{
    (void)ssh_get_u32(p);                  /* our channel, always 0 */

    char type[32];
    ssh_get_cstr(p, type, sizeof(type));
    bool want_reply = ssh_get_u8(p) != 0;
    if (p->bad) return RV9_IO_ERR_IO;

    bool ok = false;

    if (strcmp(type, "pty-req") == 0) {
        char term[32];
        ssh_get_cstr(p, term, sizeof(term));
        uint32_t cols = ssh_get_u32(p);
        uint32_t rows = ssh_get_u32(p);

        if (!p->bad && cols && rows) {
            /*
             * The first time anything on the far end of a wire has been
             * able to say how big it is. Design section 16 called 80x24 a
             * stated guess; this is what replaces it.
             */
            s->cols = cols;
            s->rows = rows;
            s->have_pty = true;
            ok = true;
            ESP_LOGI(TAG, "pty %s, %ux%u", term, (unsigned)cols, (unsigned)rows);
        }
    } else if (strcmp(type, "window-change") == 0) {
        uint32_t cols = ssh_get_u32(p);
        uint32_t rows = ssh_get_u32(p);
        if (!p->bad && cols && rows) {
            s->cols = cols;
            s->rows = rows;
            s->resized = true;
            ok = true;
        }
        want_reply = false;   /* the protocol says this one is never answered */
    } else if (strcmp(type, "shell") == 0) {
        s->shell = true;
        ok = true;
    } else if (strcmp(type, "env") == 0) {
        /* Accepted and dropped: RV-9 has no environment to put it in, and
           refusing makes clients that always send LANG look broken. */
        ok = true;
    }

    if (!want_reply) return RV9_IO_OK;
    return send_chan_reply(s, ok ? SSH_MSG_CHANNEL_SUCCESS
                                 : SSH_MSG_CHANNEL_FAILURE);
}

/*
 * One packet, whatever it is.
 *
 * The same routine runs while the session is being set up and while it is
 * carrying a shell, because the client is allowed to send most of these at
 * any time -- a window-change arrives whenever somebody drags the corner
 * of their terminal, and it must not be a surprise.
 */
static rv9_io_err_t handle_packet(ssh_t *s)
{
    if (s->pay_len == 0) return RV9_IO_ERR_IO;

    ssh_buf_t p;
    ssh_buf_load(&p, SSH_PAYLOAD(s), s->pay_len);
    uint8_t msg = ssh_get_u8(&p);

    switch (msg) {
    case SSH_MSG_IGNORE:
    case SSH_MSG_DEBUG:
    case SSH_MSG_UNIMPLEMENTED:
        return RV9_IO_OK;

    case SSH_MSG_DISCONNECT:
        s->eof = true;
        return RV9_IO_OK;

    case SSH_MSG_KEXINIT:
        /* A rekey. Nothing here can do one, and carrying on with keys the
           client has decided to replace is worse than stopping. */
        ESP_LOGW(TAG, "client asked to rekey; closing");
        ssh_disconnect(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                       "rekeying is not supported");
        s->eof = true;
        return RV9_IO_OK;

    case SSH_MSG_GLOBAL_REQUEST: {
        ssh_skip_string(&p);
        bool want_reply = ssh_get_u8(&p) != 0;
        if (p.bad || !want_reply) return RV9_IO_OK;
        return send_u8(s, SSH_MSG_REQUEST_FAILURE);
    }

    case SSH_MSG_CHANNEL_OPEN:
        return channel_open(s, &p);

    case SSH_MSG_CHANNEL_REQUEST:
        return channel_request(s, &p);

    case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
        (void)ssh_get_u32(&p);
        uint32_t add = ssh_get_u32(&p);
        if (!p.bad) s->peer_window += add;
        return RV9_IO_OK;
    }

    case SSH_MSG_CHANNEL_DATA: {
        (void)ssh_get_u32(&p);
        size_t n = 0;
        const uint8_t *d = ssh_get_string(&p, &n);
        if (p.bad || d == NULL) return RV9_IO_ERR_IO;

        /* The data is already in the packet buffer; remember where rather
           than copying it somewhere else to be read a byte at a time. */
        s->data_pos = (size_t)(d - s->in);
        s->data_end = s->data_pos + n;
        window_top_up(s, (uint32_t)n);
        return RV9_IO_OK;
    }

    case SSH_MSG_CHANNEL_EXTENDED_DATA:
        return RV9_IO_OK;    /* a client has no business sending us stderr */

    case SSH_MSG_CHANNEL_EOF:
    case SSH_MSG_CHANNEL_CLOSE:
        s->eof = true;
        return RV9_IO_OK;

    default:
        return RV9_IO_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Driver entry points                                                 */
/* ------------------------------------------------------------------ */

static rv9_io_err_t ssh_init(rv9_dev_t *dev)
{
    (void)dev;
    rv9_io_err_t err = ssh_crypto_init();
    if (err != RV9_IO_OK) return err;

    /* Making the key here rather than at the first connection keeps the
       cost off the first login, and puts the fingerprint in the boot log
       where it can be compared with what the client prints. */
    return ssh_hostkey_load();
}

static rv9_io_err_t ssh_open(rv9_dev_t *dev, uint32_t mode)
{
    (void)mode;

    if (dev->opt[OPT_CONFIG]) return RV9_IO_OK;   /* /sshcfg waits for nobody */

    if (!ssh_password_set()) {
        ESP_LOGE(TAG, "no login password set -- run `passwd` before sshd");
        return RV9_IO_ERR_MODE;
    }

    ssh_t *s = rv9_calloc(1, sizeof(*s));
    if (s == NULL) return RV9_IO_ERR_NOMEM;

    uint32_t port = dev->opt[OPT_PORT] ? dev->opt[OPT_PORT] : 22;

    char listen[32];
    int n = snprintf(listen, sizeof(listen), "/n0/listen/%u", (unsigned)port);
    if (n <= 0 || n >= (int)sizeof(listen)) {
        rv9_free(s);
        return RV9_IO_ERR_INVAL;
    }

    ESP_LOGI(TAG, "waiting for a connection on %u", (unsigned)port);

    rv9_io_err_t err = rv9_io_open_detached(listen, RV9_MODE_RW, &s->net);
    if (err != RV9_IO_OK) {
        rv9_free(s);
        return err;
    }

    err = ssh_transport(s);
    if (err == RV9_IO_OK) err = authenticate(s);

    /* Set-up messages and session messages are the same messages, so the
       same handler runs until the client asks for its shell. */
    while (err == RV9_IO_OK && !s->shell && !s->eof) {
        err = ssh_packet_read(s);
        if (err == RV9_IO_OK) err = handle_packet(s);
    }

    if (err != RV9_IO_OK || !s->shell) {
        rv9_io_close_path(s->net);
        rv9_free(s);
        return (err == RV9_IO_OK) ? RV9_IO_ERR_IO : err;
    }

    dev->drv_state = s;
    ESP_LOGI(TAG, "session up for %s", s->user);
    return RV9_IO_OK;
}

static rv9_io_err_t ssh_close(rv9_dev_t *dev)
{
    ssh_t *s = (ssh_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_OK;

    if (!s->eof && s->chan_open) {
        /* Tell the client the shell finished, so it reports an exit status
           rather than "connection closed by remote host". */
        ssh_buf_t p;
        ssh_packet_begin(s, &p);
        ssh_put_u8(&p, SSH_MSG_CHANNEL_REQUEST);
        ssh_put_u32(&p, s->peer_chan);
        ssh_put_cstr(&p, "exit-status");
        ssh_put_u8(&p, 0);
        ssh_put_u32(&p, 0);
        ssh_packet_send(s, &p);

        send_chan_reply(s, SSH_MSG_CHANNEL_EOF);
        send_chan_reply(s, SSH_MSG_CHANNEL_CLOSE);
        ssh_disconnect(s, SSH_DISCONNECT_BY_APPLICATION, "session ended");
    }

    if (s->key_c2s) psa_destroy_key(s->key_c2s);
    if (s->key_s2c) psa_destroy_key(s->key_s2c);

    rv9_io_close_path(s->net);

    dev->drv_state = NULL;
    memset(s, 0, sizeof(*s));
    rv9_free(s);

    ESP_LOGI(TAG, "session ended");
    return RV9_IO_OK;
}

static rv9_io_err_t ssh_read(rv9_dev_t *dev, void *buf, size_t len,
                             size_t *done)
{
    ssh_t *s = (ssh_t *)dev->drv_state;
    if (done) *done = 0;
    if (s == NULL) return RV9_IO_ERR_IO;
    if (len == 0) return RV9_IO_OK;

    /* Keep pumping until there is channel data. Everything else the client
       sends -- a resize, a window adjustment -- is handled on the way. */
    while (s->data_pos == s->data_end) {
        if (s->eof) return RV9_IO_ERR_IO;

        rv9_io_err_t err = ssh_packet_read(s);
        if (err != RV9_IO_OK) { s->eof = true; return err; }

        err = handle_packet(s);
        if (err != RV9_IO_OK) { s->eof = true; return err; }
    }

    size_t n = s->data_end - s->data_pos;
    if (n > len) n = len;

    memcpy(buf, s->in + s->data_pos, n);
    s->data_pos += n;

    if (done) *done = n;
    return RV9_IO_OK;
}

static rv9_io_err_t ssh_write(rv9_dev_t *dev, const void *buf, size_t len,
                              size_t *done)
{
    ssh_t *s = (ssh_t *)dev->drv_state;
    if (done) *done = 0;
    if (s == NULL || !s->chan_open) return RV9_IO_ERR_IO;
    if (s->eof) return RV9_IO_ERR_IO;

    const uint8_t *src = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        /* The client grants credit and we spend it. It normally opens with
           two megabytes, so this loop is theory for a shell -- but a
           program dumping a file would find out. */
        for (int spin = 0; s->peer_window == 0 && spin < 1000; spin++) {
            rv9_io_err_t err = ssh_packet_read(s);
            if (err != RV9_IO_OK) { s->eof = true; return err; }
            err = handle_packet(s);
            if (err != RV9_IO_OK) { s->eof = true; return err; }
            if (s->eof) return RV9_IO_ERR_IO;
        }
        if (s->peer_window == 0) return RV9_IO_ERR_TIMEOUT;

        size_t n = len - sent;
        if (n > s->peer_maxpkt)   n = s->peer_maxpkt;
        if (n > s->peer_window)   n = s->peer_window;
        if (n > SSH_BUF_MAX - 64) n = SSH_BUF_MAX - 64;

        ssh_buf_t p;
        ssh_packet_begin(s, &p);
        ssh_put_u8(&p, SSH_MSG_CHANNEL_DATA);
        ssh_put_u32(&p, s->peer_chan);
        ssh_put_string(&p, src + sent, n);

        rv9_io_err_t err = ssh_packet_send(s, &p);
        if (err != RV9_IO_OK) { s->eof = true; return err; }

        s->peer_window -= (uint32_t)n;
        sent += n;
    }

    if (done) *done = sent;
    return RV9_IO_OK;
}

static rv9_io_err_t ssh_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    ssh_t *s = (ssh_t *)dev->drv_state;

    switch (code) {
    case RV9_CON_GS_SIZE:
        /*
         * Answer only when the client actually told us. Declining lets SCF
         * fall back to its documented 80x24 guess, which is the right
         * answer for a session with no pty -- and a guess we now get to
         * stop making whenever there is one.
         */
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        if (s == NULL || !s->have_pty) return RV9_IO_ERR_UNSUPPORTED;
        *(uint32_t *)arg = (s->rows << 16) | (s->cols & 0xFFFF);
        return RV9_IO_OK;

    case RV9_SSH_GS_INFO: {
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        rv9_ssh_info_t *info = (rv9_ssh_info_t *)arg;
        memset(info, 0, sizeof(*info));

        ssh_hostkey_fingerprint(info->fingerprint, sizeof(info->fingerprint));
        info->have_password = ssh_password_set() ? 1 : 0;

        if (s != NULL) {
            strncpy(info->user, s->user, sizeof(info->user) - 1);
            info->connected = 1;
            info->have_pty  = s->have_pty ? 1 : 0;
            info->cols = (uint16_t)s->cols;
            info->rows = (uint16_t)s->rows;
        }
        return RV9_IO_OK;
    }

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static rv9_io_err_t ssh_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    (void)dev;

    switch (code) {
    case RV9_SSH_SS_PASSWORD: {
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        rv9_ssh_pw_t *pw = (rv9_ssh_pw_t *)arg;
        pw->pass[sizeof(pw->pass) - 1] = '\0';

        rv9_io_err_t err = ssh_password_store(pw->pass);
        memset(pw->pass, 0, sizeof(pw->pass));
        return err;
    }

    default:
        /*
         * Cursor, colour and the rest land here and are declined on
         * purpose. A terminal at the far end of an ssh session wants the
         * escape sequence, and SCF writes it -- exactly as it does for a
         * UART. See design section 16.
         */
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_driver_t ssh_driver = {
    .name    = "ssh",
    .init    = ssh_init,
    .open    = ssh_open,
    .close   = ssh_close,
    .read    = ssh_read,
    .write   = ssh_write,
    .getstat = ssh_getstat,
    .setstat = ssh_setstat,
};

rv9_io_err_t rv9_drv_ssh_register(void)
{
    return rv9_io_register_driver(&ssh_driver);
}
