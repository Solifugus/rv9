/*
 * SSH, from the inside.
 *
 * Private to the component. Three files share it: ssh_crypto.c owns the
 * algorithms and the host key, ssh_trans.c owns packets and key exchange,
 * and drv_ssh.c owns the session and presents it as a character device.
 *
 * One suite, no negotiation to speak of:
 *
 *   key exchange    curve25519-sha256
 *   host key        ecdsa-sha2-nistp256
 *   cipher          aes256-gcm@openssh.com
 *
 * Two curves rather than one because mbedTLS 4 has no Ed25519 in this
 * build; X25519 is what the key exchange wants and P-256 is what is left
 * for signing. All three are in OpenSSH's defaults, so a stock client
 * connects without being told anything, and all three are accelerated in
 * hardware on this chip.
 *
 * What is deliberately absent: rekeying, compression, more than one
 * channel, more than one session at a time, and any cipher that is not an
 * AEAD. A second implementation of anything is a second thing to get
 * wrong, and this one is small enough to read.
 */
#pragma once

#include "rv9/io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

/*
 * The largest packet we will handle.
 *
 * The protocol allows 32 KB; a client's KEXINIT is the only thing that
 * comes close, and OpenSSH's runs to about 1.5 KB of algorithm names.
 * Channel data is bounded separately by the maximum packet we advertise.
 *
 * The number matters more than it looks. The session holds four buffers of
 * roughly this size in one allocation, and at 4 KB each that came to
 * sixteen kilobytes of *contiguous* heap -- on a board with fifty free and
 * a WiFi stack scattered through it. The allocation failed often enough
 * that roughly two connections in three were refused, silently, because
 * the daemon simply went back to waiting.
 */
#define SSH_BUF_MAX       2560

#define SSH_HASH_LEN      32     /* SHA-256 */
#define SSH_KEY_LEN       32     /* AES-256 */
#define SSH_IV_LEN        12     /* GCM: 4 fixed || 8 counter */
#define SSH_TAG_LEN       16
#define SSH_BLOCK_LEN     16
#define SSH_X25519_LEN    32
#define SSH_HOSTPUB_LEN   65     /* 0x04 || X || Y */
#define SSH_HOSTPRIV_LEN  32
#define SSH_SIG_LEN       64     /* r || s, raw */

#define SSH_MIN_PAD       4

/*
 * What we advertise when the session channel opens.
 *
 * The window is credit, and credit is what stops the client sending more
 * than there is anywhere to put. It is returned as data is *consumed*, not
 * as it arrives -- returning it on arrival advertises room that does not
 * exist yet, which is the same as having no flow control at all.
 *
 * So the window is the size of the buffer behind it, and the buffer has
 * one whole packet of slack on top.
 */
#define SSH_WINDOW_INIT   1024u
#define SSH_MAX_PAYLOAD   1024u
#define SSH_PEND_MAX      (SSH_WINDOW_INIT + SSH_MAX_PAYLOAD)

/* ------------------------------------------------------------------ */
/* Message numbers                                                     */
/* ------------------------------------------------------------------ */

#define SSH_MSG_DISCONNECT                  1
#define SSH_MSG_IGNORE                      2
#define SSH_MSG_UNIMPLEMENTED               3
#define SSH_MSG_DEBUG                       4
#define SSH_MSG_SERVICE_REQUEST             5
#define SSH_MSG_SERVICE_ACCEPT              6
#define SSH_MSG_EXT_INFO                    7
#define SSH_MSG_KEXINIT                    20
#define SSH_MSG_NEWKEYS                    21
#define SSH_MSG_KEX_ECDH_INIT              30
#define SSH_MSG_KEX_ECDH_REPLY             31
#define SSH_MSG_USERAUTH_REQUEST           50
#define SSH_MSG_USERAUTH_FAILURE           51
#define SSH_MSG_USERAUTH_SUCCESS           52
#define SSH_MSG_USERAUTH_BANNER            53
#define SSH_MSG_USERAUTH_PK_OK             60   /* method-specific */
#define SSH_MSG_GLOBAL_REQUEST             80
#define SSH_MSG_REQUEST_SUCCESS            81
#define SSH_MSG_REQUEST_FAILURE            82
#define SSH_MSG_CHANNEL_OPEN               90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION  91
#define SSH_MSG_CHANNEL_OPEN_FAILURE       92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST      93
#define SSH_MSG_CHANNEL_DATA               94
#define SSH_MSG_CHANNEL_EXTENDED_DATA      95
#define SSH_MSG_CHANNEL_EOF                96
#define SSH_MSG_CHANNEL_CLOSE              97
#define SSH_MSG_CHANNEL_REQUEST            98
#define SSH_MSG_CHANNEL_SUCCESS            99
#define SSH_MSG_CHANNEL_FAILURE           100

#define SSH_DISCONNECT_PROTOCOL_ERROR              2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED         3
#define SSH_DISCONNECT_MAC_ERROR                   5
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE       7
#define SSH_DISCONNECT_BY_APPLICATION             11
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS       14

/* ------------------------------------------------------------------ */
/* Reading and writing the wire format                                 */
/* ------------------------------------------------------------------ */

/*
 * A cursor over a byte buffer, with one sticky error flag.
 *
 * Every get_ and put_ below checks `bad` first and sets it on overrun,
 * which means a caller can parse a whole message without checking
 * anything, then check once. The alternative -- a return value per field,
 * tested every time -- is how parsers of untrusted input come to have an
 * unchecked path in them.
 */
typedef struct {
    uint8_t *b;
    size_t   cap;
    size_t   len;   /* bytes written */
    size_t   pos;   /* read cursor */
    bool     bad;
} ssh_buf_t;

static inline void ssh_buf_init(ssh_buf_t *s, uint8_t *b, size_t cap)
{
    s->b = b; s->cap = cap; s->len = 0; s->pos = 0; s->bad = false;
}

static inline void ssh_buf_load(ssh_buf_t *s, uint8_t *b, size_t len)
{
    s->b = b; s->cap = len; s->len = len; s->pos = 0; s->bad = false;
}

static inline void ssh_put(ssh_buf_t *s, const void *p, size_t n)
{
    if (s->bad || s->len + n > s->cap) { s->bad = true; return; }
    memcpy(s->b + s->len, p, n);
    s->len += n;
}

static inline void ssh_put_u8(ssh_buf_t *s, uint8_t v)
{
    ssh_put(s, &v, 1);
}

static inline void ssh_put_u32(ssh_buf_t *s, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    ssh_put(s, t, 4);
}

/* A "string" on the wire is a length and then bytes, never NUL-terminated. */
static inline void ssh_put_string(ssh_buf_t *s, const void *p, size_t n)
{
    ssh_put_u32(s, (uint32_t)n);
    ssh_put(s, p, n);
}

static inline void ssh_put_cstr(ssh_buf_t *s, const char *p)
{
    ssh_put_string(s, p, strlen(p));
}

/*
 * An mpint is a signed big-endian integer, so a value whose top bit is set
 * needs a leading zero byte or it would be read as negative. Leading zero
 * bytes are otherwise stripped. Getting this wrong produces an exchange
 * hash that differs from the client's by one byte, roughly once in every
 * two hundred and fifty six connections.
 */
static inline void ssh_put_mpint(ssh_buf_t *s, const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n && p[i] == 0) i++;

    if (i == n) { ssh_put_u32(s, 0); return; }

    bool pad = (p[i] & 0x80) != 0;
    ssh_put_u32(s, (uint32_t)(n - i + (pad ? 1 : 0)));
    if (pad) ssh_put_u8(s, 0);
    ssh_put(s, p + i, n - i);
}

static inline uint8_t ssh_get_u8(ssh_buf_t *s)
{
    if (s->bad || s->pos + 1 > s->len) { s->bad = true; return 0; }
    return s->b[s->pos++];
}

static inline uint32_t ssh_get_u32(ssh_buf_t *s)
{
    if (s->bad || s->pos + 4 > s->len) { s->bad = true; return 0; }
    uint32_t v = ((uint32_t)s->b[s->pos] << 24) |
                 ((uint32_t)s->b[s->pos + 1] << 16) |
                 ((uint32_t)s->b[s->pos + 2] << 8) |
                  (uint32_t)s->b[s->pos + 3];
    s->pos += 4;
    return v;
}

/* Points into the buffer; the caller must not keep it past the packet. */
static inline const uint8_t *ssh_get_string(ssh_buf_t *s, size_t *n)
{
    uint32_t len = ssh_get_u32(s);
    if (s->bad || len > s->len - s->pos) { s->bad = true; *n = 0; return NULL; }
    const uint8_t *p = s->b + s->pos;
    s->pos += len;
    *n = len;
    return p;
}

static inline void ssh_skip_string(ssh_buf_t *s)
{
    size_t n = 0;
    (void)ssh_get_string(s, &n);
}

/* Copy a wire string out as a C string, truncating rather than overflowing. */
static inline void ssh_get_cstr(ssh_buf_t *s, char *out, size_t cap)
{
    size_t n = 0;
    const uint8_t *p = ssh_get_string(s, &n);
    if (s->bad || cap == 0) { if (cap) out[0] = '\0'; return; }
    if (n > cap - 1) n = cap - 1;
    if (p) memcpy(out, p, n);
    out[n] = '\0';
}

/* Is `name` one of the comma-separated names in this wire string? */
bool ssh_name_in_list(const uint8_t *list, size_t len, const char *name);

/* ------------------------------------------------------------------ */
/* The session                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    rv9_path_t *net;          /* the transport, a detached path */

    uint32_t seq_in, seq_out;
    bool     enc_in, enc_out; /* the two directions switch on separately:
                                 each side sends NEWKEYS and only then
                                 starts encrypting */
    bool     strict_kex;      /* client asked for it, so sequence numbers
                                 restart at NEWKEYS */
    bool     want_ext_info;   /* client sent ext-info-c, so it will read
                                 the list of signatures we can verify */
    bool     pk_ok;           /* last key offer was answered with PK_OK */

    psa_key_id_t key_c2s, key_s2c;
    uint8_t      iv_c2s[SSH_IV_LEN], iv_s2c[SSH_IV_LEN];
    uint64_t     ctr_c2s, ctr_s2c;

    uint8_t  session_id[SSH_HASH_LEN];

    /* Kept only until the exchange hash has absorbed them. */
    char     v_c[256];
    uint8_t  i_s[512];
    size_t   i_s_len;

    /* The one channel. */
    bool     chan_open;
    bool     shell;

    /*
     * Two different endings, and conflating them cost an afternoon.
     *
     * in_eof is the client saying it has no more *input* -- which a client
     * whose stdin is a pipe says immediately, long before the session is
     * over. Reads should report the end of input; writes must keep
     * working, or everything the far side prints after that moment is
     * thrown away.
     *
     * eof is the channel or the connection actually being finished.
     */
    bool     in_eof;
    bool     eof;
    uint32_t peer_chan;
    uint32_t peer_window;
    uint32_t peer_maxpkt;
    uint32_t my_window;

    /* What the client says its terminal is, from pty-req and
       window-change. This is the only honest answer to RV9_CON_GS_SIZE
       that anything on the far end of a wire has ever been able to give. */
    uint32_t cols, rows;
    bool     have_pty;
    bool     resized;

    char     user[32];

    /*
     * The incoming packet.
     *
     * `in` holds the packet with its padding length byte still on the
     * front -- which is where it sits in both the encrypted and the plain
     * framing, so the payload is always at `in + 1` and nothing has to be
     * moved after decrypting.
     */
    uint8_t  in[SSH_BUF_MAX];
    size_t   pay_len;

    /*
     * Channel data waiting to be read, copied out of the packet.
     *
     * It used to be a pair of offsets into `in`, which costs no memory and
     * is wrong: reading the next packet overwrites `in`. A client whose
     * stdin is a pipe sends its whole input and an EOF *during* channel
     * setup, before the shell exists to read any of it, and every byte of
     * it was destroyed by the packet that followed. Interactive sessions
     * never noticed, because a terminal sends nothing until there is
     * something to type at.
     */
    uint8_t  pend[SSH_PEND_MAX];
    size_t   pend_pos, pend_len;

    /*
     * Outgoing channel data, gathered up.
     *
     * SCF echoes as it reads, so an interactive session writes one or two
     * bytes at a time -- and a byte at a time straight down the wire is a
     * whole SSH packet, a whole TCP segment, and forty-odd bytes of header
     * for every character typed. Twenty-five segments for one command.
     *
     * They are collected here and sent when the program stops talking:
     * before it blocks for input, and when the session ends. That is the
     * moment its output is complete, and it is the same rule a terminal
     * driver has always used.
     */
    uint8_t  obuf[SSH_MAX_PAYLOAD];
    size_t   obuf_len;

    /*
     * The outgoing packet, with the same one byte reserved at the front,
     * so a payload is built where it will be framed and encrypted rather
     * than copied there afterwards.
     */
    uint8_t  out[SSH_BUF_MAX];

    /* On the wire: length, ciphertext and tag. Big enough for either
       direction, and here rather than on the stack because a process
       running the shell does not have four kilobytes to spare. */
    uint8_t  frame[SSH_BUF_MAX + 64];
} ssh_t;

#define SSH_PAYLOAD(s) ((s)->in + 1)

static inline void ssh_packet_begin(ssh_t *s, ssh_buf_t *p)
{
    ssh_buf_init(p, s->out + 1, sizeof(s->out) - 1);
}

/* ---- ssh_crypto.c ---- */

rv9_io_err_t ssh_crypto_init(void);

/* The host key, loaded from NVS or generated and stored on first use. */
rv9_io_err_t ssh_hostkey_load(void);
void         ssh_hostkey_blob(ssh_buf_t *s);            /* K_S, as a string */
rv9_io_err_t ssh_hostkey_sign(const uint8_t hash[SSH_HASH_LEN],
                              ssh_buf_t *s);            /* signature string */
void         ssh_hostkey_fingerprint(char *out, size_t cap);

/* The login password, salted and hashed in NVS. */
bool         ssh_password_set(void);
rv9_io_err_t ssh_password_store(const char *password);
bool         ssh_password_check(const char *password);

/* ---- ssh_auth.c ---- */

/* Is this exact key blob listed in /f0/authkeys? */
bool ssh_authkey_allowed(const uint8_t *blob, size_t blob_len);

/* Verify a signature over `data` using the public key in `blob`. The key's
   type comes from inside the blob, not from anything the client claimed
   beside it. */
rv9_io_err_t ssh_pubkey_verify(const uint8_t *blob, size_t blob_len,
                               const char *sig_alg,
                               const uint8_t *sig, size_t sig_len,
                               const uint8_t *data, size_t data_len);

/* What we will verify, offered to the client as server-sig-algs so that a
   modern client is willing to try an RSA key at all. */
#define SSH_SIG_ALGS "ecdsa-sha2-nistp256,rsa-sha2-256,rsa-sha2-512"

/* ---- ssh_trans.c ---- */

rv9_io_err_t ssh_transport(ssh_t *s);   /* version exchange through NEWKEYS */

/* Read one packet into s->in; payload is s->in[0 .. s->in_len). */
rv9_io_err_t ssh_packet_read(ssh_t *s);

/* Send the payload built in `p` (which must be over s->out). */
rv9_io_err_t ssh_packet_send(ssh_t *s, ssh_buf_t *p);

void ssh_disconnect(ssh_t *s, uint32_t reason, const char *text);
