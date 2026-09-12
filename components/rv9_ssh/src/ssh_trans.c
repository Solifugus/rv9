/*
 * The transport layer: framing, key exchange, and the moment everything
 * after it is encrypted.
 *
 * Nothing here knows what a socket is. The connection arrives as a path
 * and is read and written like any other path, which means SSH would run
 * over anything the I/O system can open -- another board, a serial line --
 * without a line of this file changing.
 */
#include "ssh.h"

#include "rv9/kal.h"

#include "esp_log.h"

static const char *TAG = "rv9-ssh";

#define SSH_VERSION "SSH-2.0-RV9_0.1"

#define KEX_NAMES      "curve25519-sha256,curve25519-sha256@libssh.org," \
                       "kex-strict-s-v00@openssh.com"
#define HOSTKEY_NAMES  "ecdsa-sha2-nistp256"
#define CIPHER_NAMES   "aes256-gcm@openssh.com"
#define MAC_NAMES      "hmac-sha2-256"
#define COMP_NAMES     "none"

#define KEX_WANT     "curve25519-sha256"
#define KEX_STRICT_C "kex-strict-c-v00@openssh.com"

/* ------------------------------------------------------------------ */
/* Moving bytes over the path underneath                               */
/* ------------------------------------------------------------------ */

static rv9_io_err_t read_exact(ssh_t *s, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t moved = 0;
        rv9_io_err_t err = rv9_io_read_path(s->net, buf + got, n - got, &moved);

        if (err == RV9_IO_ERR_WOULDBLOCK || (err == RV9_IO_OK && moved == 0)) {
            rv9_task_delay_ms(5);
            continue;
        }
        if (err != RV9_IO_OK) return err;
        got += moved;
    }
    return RV9_IO_OK;
}

static rv9_io_err_t write_all(ssh_t *s, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        size_t moved = 0;
        rv9_io_err_t err = rv9_io_write_path(s->net, buf + sent, n - sent,
                                             &moved);
        if (err == RV9_IO_ERR_WOULDBLOCK) {
            rv9_task_delay_ms(5);
            continue;
        }
        if (err != RV9_IO_OK) return err;
        if (moved == 0) { rv9_task_delay_ms(5); continue; }
        sent += moved;
    }
    return RV9_IO_OK;
}

/* ------------------------------------------------------------------ */
/* Name lists                                                          */
/* ------------------------------------------------------------------ */

bool ssh_name_in_list(const uint8_t *list, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    size_t i = 0;

    while (i < len) {
        size_t j = i;
        while (j < len && list[j] != ',') j++;
        if (j - i == nlen && memcmp(list + i, name, nlen) == 0) return true;
        i = j + 1;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Packets                                                             */
/* ------------------------------------------------------------------ */

static void gcm_nonce(const uint8_t iv[SSH_IV_LEN], uint64_t ctr,
                      uint8_t out[SSH_IV_LEN])
{
    memcpy(out, iv, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
}

/*
 * Send one packet.
 *
 * Padding is at least four bytes and brings the encrypted part to a
 * multiple of the block size. Under AES-GCM the length field is outside
 * the encryption and authenticated as associated data instead, so it is
 * the *encrypted* part alone that has to come out even -- which is the one
 * place this framing differs from the classic one, and the one place an
 * implementation written from the older RFC gets it wrong.
 */
rv9_io_err_t ssh_packet_send(ssh_t *s, ssh_buf_t *p)
{
    if (p->bad) return RV9_IO_ERR_INVAL;
    if (p->b != s->out + 1) return RV9_IO_ERR_INVAL;   /* not from begin() */

    size_t payload = p->len;
    size_t block   = s->enc_out ? SSH_BLOCK_LEN : 8;

    size_t pad;
    if (s->enc_out) {
        pad = block - ((1 + payload) % block);
    } else {
        pad = block - ((4 + 1 + payload) % block);
    }
    if (pad < SSH_MIN_PAD) pad += block;

    size_t plain_len = 1 + payload + pad;

    if (plain_len > sizeof(s->out)) return RV9_IO_ERR_INVAL;
    if (4 + plain_len + SSH_TAG_LEN > sizeof(s->frame)) return RV9_IO_ERR_INVAL;

    s->frame[0] = (uint8_t)(plain_len >> 24);
    s->frame[1] = (uint8_t)(plain_len >> 16);
    s->frame[2] = (uint8_t)(plain_len >> 8);
    s->frame[3] = (uint8_t)plain_len;

    /* The payload is already at out+1; only the padding length in front of
       it and the random padding behind it are still missing. */
    s->out[0] = (uint8_t)pad;
    if (psa_generate_random(s->out + 1 + payload, pad) != PSA_SUCCESS) {
        return RV9_IO_ERR_IO;
    }

    size_t total;
    if (s->enc_out) {
        uint8_t nonce[SSH_IV_LEN];
        gcm_nonce(s->iv_s2c, s->ctr_s2c, nonce);

        size_t n = 0;
        if (psa_aead_encrypt(s->key_s2c, PSA_ALG_GCM, nonce, sizeof(nonce),
                             s->frame, 4, s->out, plain_len,
                             s->frame + 4, sizeof(s->frame) - 4, &n)
                != PSA_SUCCESS) {
            return RV9_IO_ERR_IO;
        }
        s->ctr_s2c++;
        total = 4 + n;
    } else {
        memcpy(s->frame + 4, s->out, plain_len);
        total = 4 + plain_len;
    }

    s->seq_out++;
    return write_all(s, s->frame, total);
}

/*
 * Read one packet. On success the payload is at SSH_PAYLOAD(s), s->pay_len
 * bytes long.
 */
rv9_io_err_t ssh_packet_read(ssh_t *s)
{
    uint8_t lenbuf[4];
    rv9_io_err_t err = read_exact(s, lenbuf, 4);
    if (err != RV9_IO_OK) return err;

    uint32_t plen = ((uint32_t)lenbuf[0] << 24) | ((uint32_t)lenbuf[1] << 16) |
                    ((uint32_t)lenbuf[2] << 8) | lenbuf[3];

    /*
     * Before the cipher is on, the block size is eight -- so the smallest
     * legal packet is twelve bytes, not sixteen. A NEWKEYS carries one
     * byte of payload and comes in at exactly twelve, which makes this the
     * check that decides whether a handshake ever finishes.
     */
    uint32_t min = s->enc_in ? SSH_BLOCK_LEN : 8;

    if (plen < min || plen > SSH_BUF_MAX) {
        ESP_LOGW(TAG, "packet length %u out of range", (unsigned)plen);
        return RV9_IO_ERR_IO;
    }

    if (s->enc_in) {
        if (plen % SSH_BLOCK_LEN != 0) {
            ESP_LOGW(TAG, "packet length %u not a whole number of blocks",
                     (unsigned)plen);
            return RV9_IO_ERR_IO;
        }

        uint8_t *ct = s->frame;
        err = read_exact(s, ct, plen + SSH_TAG_LEN);
        if (err != RV9_IO_OK) return err;

        uint8_t nonce[SSH_IV_LEN];
        gcm_nonce(s->iv_c2s, s->ctr_c2s, nonce);

        size_t n = 0;
        if (psa_aead_decrypt(s->key_c2s, PSA_ALG_GCM, nonce, sizeof(nonce),
                             lenbuf, 4, ct, plen + SSH_TAG_LEN,
                             s->in, sizeof(s->in), &n) != PSA_SUCCESS) {
            ESP_LOGW(TAG, "packet failed authentication");
            return RV9_IO_ERR_IO;
        }
        s->ctr_c2s++;

        if (n != plen) return RV9_IO_ERR_IO;
    } else {
        if (plen > sizeof(s->in)) return RV9_IO_ERR_IO;
        err = read_exact(s, s->in, plen);
        if (err != RV9_IO_OK) return err;
    }

    uint8_t pad = s->in[0];
    if ((size_t)pad + 1 > plen) return RV9_IO_ERR_IO;

    s->pay_len = plen - pad - 1;
    s->seq_in++;
    return RV9_IO_OK;
}

void ssh_disconnect(ssh_t *s, uint32_t reason, const char *text)
{
    ssh_buf_t p;
    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, SSH_MSG_DISCONNECT);
    ssh_put_u32(&p, reason);
    ssh_put_cstr(&p, text);
    ssh_put_cstr(&p, "");
    ssh_packet_send(s, &p);
}

/* ------------------------------------------------------------------ */
/* Version exchange                                                    */
/* ------------------------------------------------------------------ */

/*
 * Both sides announce themselves in plain text, and both sides have to
 * remember exactly what the other said: the version strings go into the
 * exchange hash, so a byte changed here is a handshake that fails there.
 *
 * A client may send any number of other lines first. They are not part of
 * the hash and are ignored.
 */
static rv9_io_err_t version_exchange(ssh_t *s)
{
    rv9_io_err_t err = write_all(s, (const uint8_t *)SSH_VERSION "\r\n",
                                 strlen(SSH_VERSION) + 2);
    if (err != RV9_IO_OK) return err;

    for (int line = 0; line < 32; line++) {
        size_t n = 0;
        for (;;) {
            uint8_t c;
            err = read_exact(s, &c, 1);
            if (err != RV9_IO_OK) return err;

            if (c == '\n') break;
            if (c == '\r') continue;
            if (n < sizeof(s->v_c) - 1) s->v_c[n++] = (char)c;
        }
        s->v_c[n] = '\0';

        if (n >= 4 && memcmp(s->v_c, "SSH-", 4) == 0) {
            if (memcmp(s->v_c, "SSH-2.0", 7) != 0 &&
                memcmp(s->v_c, "SSH-1.99", 8) != 0) {
                ESP_LOGW(TAG, "client speaks '%s', which we do not", s->v_c);
                return RV9_IO_ERR_UNSUPPORTED;
            }
            ESP_LOGI(TAG, "client is %s", s->v_c);
            return RV9_IO_OK;
        }
    }

    return RV9_IO_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* Key exchange                                                        */
/* ------------------------------------------------------------------ */

static rv9_io_err_t send_kexinit(ssh_t *s)
{
    ssh_buf_t p;
    ssh_packet_begin(s, &p);

    ssh_put_u8(&p, SSH_MSG_KEXINIT);

    uint8_t cookie[16];
    if (psa_generate_random(cookie, sizeof(cookie)) != PSA_SUCCESS) {
        return RV9_IO_ERR_IO;
    }
    ssh_put(&p, cookie, sizeof(cookie));

    ssh_put_cstr(&p, KEX_NAMES);
    ssh_put_cstr(&p, HOSTKEY_NAMES);
    ssh_put_cstr(&p, CIPHER_NAMES);     /* client to server */
    ssh_put_cstr(&p, CIPHER_NAMES);     /* server to client */
    ssh_put_cstr(&p, MAC_NAMES);        /* ignored: the cipher is an AEAD */
    ssh_put_cstr(&p, MAC_NAMES);
    ssh_put_cstr(&p, COMP_NAMES);
    ssh_put_cstr(&p, COMP_NAMES);
    ssh_put_cstr(&p, "");               /* languages */
    ssh_put_cstr(&p, "");
    ssh_put_u8(&p, 0);                  /* no guess follows */
    ssh_put_u32(&p, 0);

    if (p.bad || p.len > sizeof(s->i_s)) return RV9_IO_ERR_INVAL;

    /* Keep it: the exchange hash covers both sides' KEXINIT verbatim. */
    memcpy(s->i_s, p.b, p.len);
    s->i_s_len = p.len;

    return ssh_packet_send(s, &p);
}

/*
 * What the client offered, against the one thing we do.
 *
 * Checking rather than assuming costs fifteen lines and turns "the
 * connection closed" into a log line naming the algorithm that was
 * missing, which is the difference between a five-minute problem and an
 * evening with a packet capture.
 */
static rv9_io_err_t check_kexinit(ssh_t *s, bool *guess_follows)
{
    ssh_buf_t p;
    ssh_buf_load(&p, SSH_PAYLOAD(s), s->pay_len);

    if (ssh_get_u8(&p) != SSH_MSG_KEXINIT) return RV9_IO_ERR_IO;
    p.pos += 16;   /* cookie */

    size_t n = 0;
    const uint8_t *kex = ssh_get_string(&p, &n);
    size_t kex_len = n;

    const uint8_t *hostkey = ssh_get_string(&p, &n);
    size_t hostkey_len = n;

    const uint8_t *enc_cs = ssh_get_string(&p, &n);
    size_t enc_cs_len = n;

    const uint8_t *enc_sc = ssh_get_string(&p, &n);
    size_t enc_sc_len = n;

    ssh_skip_string(&p);   /* mac c2s */
    ssh_skip_string(&p);   /* mac s2c */
    ssh_skip_string(&p);   /* comp c2s */
    ssh_skip_string(&p);   /* comp s2c */
    ssh_skip_string(&p);   /* lang c2s */
    ssh_skip_string(&p);   /* lang s2c */

    bool guessed = (ssh_get_u8(&p) != 0);

    if (p.bad) return RV9_IO_ERR_IO;

    /*
     * A client may send its guess at the key exchange immediately after
     * KEXINIT, and that packet is only to be discarded if it guessed
     * *wrong* -- which it did unless its first choice is also ours. OpenSSH
     * does not guess, so this is rarely exercised; throwing the packet away
     * unconditionally would be a bug that waits for a client that does.
     */
    size_t first = 0;
    while (first < kex_len && kex[first] != ',') first++;
    *guess_follows = guessed &&
                     !(first == strlen(KEX_WANT) &&
                       memcmp(kex, KEX_WANT, first) == 0);

    if (!ssh_name_in_list(kex, kex_len, KEX_WANT) &&
        !ssh_name_in_list(kex, kex_len, "curve25519-sha256@libssh.org")) {
        ESP_LOGW(TAG, "client will not do %s", KEX_WANT);
        return RV9_IO_ERR_UNSUPPORTED;
    }
    if (!ssh_name_in_list(hostkey, hostkey_len, HOSTKEY_NAMES)) {
        ESP_LOGW(TAG, "client will not accept an %s host key", HOSTKEY_NAMES);
        return RV9_IO_ERR_UNSUPPORTED;
    }
    if (!ssh_name_in_list(enc_cs, enc_cs_len, CIPHER_NAMES) ||
        !ssh_name_in_list(enc_sc, enc_sc_len, CIPHER_NAMES)) {
        ESP_LOGW(TAG, "client will not do %s", CIPHER_NAMES);
        return RV9_IO_ERR_UNSUPPORTED;
    }

    s->strict_kex    = ssh_name_in_list(kex, kex_len, KEX_STRICT_C);
    s->want_ext_info = ssh_name_in_list(kex, kex_len, "ext-info-c");
    return RV9_IO_OK;
}

/*
 * Tell the client which signature algorithms we can verify.
 *
 * Without this, an OpenSSH client assumes a server that has not said
 * otherwise can only do ssh-rsa -- which is SHA-1, which it disabled in
 * 8.8 -- and so will not offer an RSA key at all. The key would be in
 * authorized_keys, the client would hold the private half, and
 * authentication would fail without either end saying anything useful.
 */
static rv9_io_err_t send_ext_info(ssh_t *s)
{
    ssh_buf_t p;
    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, SSH_MSG_EXT_INFO);
    ssh_put_u32(&p, 1);
    ssh_put_cstr(&p, "server-sig-algs");
    ssh_put_cstr(&p, SSH_SIG_ALGS);
    return ssh_packet_send(s, &p);
}

/* Everything hashed into H is a length-prefixed string, K excepted. */
static void hash_string(psa_hash_operation_t *op, const void *p, size_t n)
{
    uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16),
                       (uint8_t)(n >> 8), (uint8_t)n };
    psa_hash_update(op, len, 4);
    psa_hash_update(op, (const uint8_t *)p, n);
}

/*
 * Derive one key.
 *
 *   K1 = HASH(K || H || letter || session_id)
 *
 * K carries its mpint encoding into the hash, H and the session id do not:
 * they go in as the raw thirty-two bytes. Six keys come out of this, of
 * which an AEAD cipher uses four -- the two MAC keys have nothing to do.
 */
static void derive_key(const uint8_t *k_mpint, size_t k_len,
                       const uint8_t h[SSH_HASH_LEN],
                       const uint8_t sid[SSH_HASH_LEN],
                       char letter, uint8_t *out, size_t out_len)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    uint8_t full[SSH_HASH_LEN];
    size_t  n = 0;

    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, k_mpint, k_len);
    psa_hash_update(&op, h, SSH_HASH_LEN);
    psa_hash_update(&op, (const uint8_t *)&letter, 1);
    psa_hash_update(&op, sid, SSH_HASH_LEN);
    psa_hash_finish(&op, full, sizeof(full), &n);

    if (out_len > sizeof(full)) out_len = sizeof(full);
    memcpy(out, full, out_len);
    memset(full, 0, sizeof(full));
}

static rv9_io_err_t install_key(const uint8_t key[SSH_KEY_LEN],
                                psa_key_usage_t usage, psa_key_id_t *out)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, usage);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);

    psa_status_t st = psa_import_key(&a, key, SSH_KEY_LEN, out);
    psa_reset_key_attributes(&a);
    return (st == PSA_SUCCESS) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

static uint64_t ctr_from_iv(const uint8_t iv[SSH_IV_LEN])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | iv[4 + i];
    return v;
}

rv9_io_err_t ssh_transport(ssh_t *s)
{
    rv9_io_err_t err = version_exchange(s);
    if (err != RV9_IO_OK) return err;

    err = send_kexinit(s);
    if (err != RV9_IO_OK) return err;

    err = ssh_packet_read(s);
    if (err != RV9_IO_OK) return err;
    if (s->pay_len == 0 || SSH_PAYLOAD(s)[0] != SSH_MSG_KEXINIT) {
        return RV9_IO_ERR_IO;
    }

    bool guess_follows = false;
    err = check_kexinit(s, &guess_follows);
    if (err != RV9_IO_OK) {
        ssh_disconnect(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                       "no algorithm in common");
        return err;
    }

    /*
     * Start the exchange hash now, while the client's KEXINIT is still in
     * the buffer. Hashing it in place is what lets the session hold one
     * packet buffer instead of a copy of every message that goes into H.
     */
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&hash, PSA_ALG_SHA_256);
    hash_string(&hash, s->v_c, strlen(s->v_c));
    hash_string(&hash, SSH_VERSION, strlen(SSH_VERSION));
    hash_string(&hash, SSH_PAYLOAD(s), s->pay_len);
    hash_string(&hash, s->i_s, s->i_s_len);

    /* A client that guessed the key exchange wrong sent a packet we must
       throw away before the real one. */
    if (guess_follows) {
        err = ssh_packet_read(s);
        if (err != RV9_IO_OK) return err;
    }

    err = ssh_packet_read(s);
    if (err != RV9_IO_OK) return err;

    ssh_buf_t p;
    ssh_buf_load(&p, SSH_PAYLOAD(s), s->pay_len);
    if (ssh_get_u8(&p) != SSH_MSG_KEX_ECDH_INIT) return RV9_IO_ERR_IO;

    size_t qc_len = 0;
    const uint8_t *q_c = ssh_get_string(&p, &qc_len);
    if (p.bad || qc_len != SSH_X25519_LEN) return RV9_IO_ERR_IO;

    /* Our ephemeral half. PSA exports X25519 as thirty-two raw bytes,
       which is the wire format, so Q_S needs no conversion. */
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);

    psa_key_id_t eph = 0;
    psa_status_t st = psa_generate_key(&a, &eph);
    psa_reset_key_attributes(&a);
    if (st != PSA_SUCCESS) return RV9_IO_ERR_IO;

    uint8_t q_s[SSH_X25519_LEN];
    size_t  n = 0;
    st = psa_export_public_key(eph, q_s, sizeof(q_s), &n);
    if (st != PSA_SUCCESS || n != SSH_X25519_LEN) {
        psa_destroy_key(eph);
        return RV9_IO_ERR_IO;
    }

    uint8_t secret[SSH_X25519_LEN];
    st = psa_raw_key_agreement(PSA_ALG_ECDH, eph, q_c, qc_len,
                               secret, sizeof(secret), &n);
    psa_destroy_key(eph);
    if (st != PSA_SUCCESS || n != SSH_X25519_LEN) return RV9_IO_ERR_IO;

    /* K goes into the hash as an mpint, and into the key derivation as the
       same bytes -- encoding included. */
    uint8_t k_mpint[SSH_X25519_LEN + 8];
    ssh_buf_t kb;
    ssh_buf_init(&kb, k_mpint, sizeof(k_mpint));
    ssh_put_mpint(&kb, secret, sizeof(secret));
    memset(secret, 0, sizeof(secret));
    if (kb.bad) return RV9_IO_ERR_IO;

    uint8_t ksblob[128];
    ssh_buf_t kbuf;
    ssh_buf_init(&kbuf, ksblob, sizeof(ksblob));
    ssh_hostkey_blob(&kbuf);
    if (kbuf.bad) return RV9_IO_ERR_IO;

    /* K_S is already a string; the rest still need their lengths. */
    psa_hash_update(&hash, kbuf.b, kbuf.len);
    hash_string(&hash, q_c, qc_len);
    hash_string(&hash, q_s, sizeof(q_s));
    psa_hash_update(&hash, k_mpint, kb.len);

    uint8_t h[SSH_HASH_LEN];
    if (psa_hash_finish(&hash, h, sizeof(h), &n) != PSA_SUCCESS) {
        return RV9_IO_ERR_IO;
    }

    /* The first exchange hash is the session identifier, for good. */
    memcpy(s->session_id, h, SSH_HASH_LEN);

    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, SSH_MSG_KEX_ECDH_REPLY);
    ssh_put(&p, kbuf.b, kbuf.len);              /* K_S, string already */
    ssh_put_string(&p, q_s, sizeof(q_s));
    err = ssh_hostkey_sign(h, &p);
    if (err != RV9_IO_OK) return err;

    err = ssh_packet_send(s, &p);
    if (err != RV9_IO_OK) return err;

    /* Derive before switching, so nothing is sent in the gap. */
    uint8_t iv_cs[SSH_IV_LEN], iv_sc[SSH_IV_LEN];
    uint8_t key_cs[SSH_KEY_LEN], key_sc[SSH_KEY_LEN];

    derive_key(k_mpint, kb.len, h, s->session_id, 'A', iv_cs, sizeof(iv_cs));
    derive_key(k_mpint, kb.len, h, s->session_id, 'B', iv_sc, sizeof(iv_sc));
    derive_key(k_mpint, kb.len, h, s->session_id, 'C', key_cs, sizeof(key_cs));
    derive_key(k_mpint, kb.len, h, s->session_id, 'D', key_sc, sizeof(key_sc));
    memset(k_mpint, 0, sizeof(k_mpint));

    err = install_key(key_cs, PSA_KEY_USAGE_DECRYPT, &s->key_c2s);
    if (err == RV9_IO_OK) {
        err = install_key(key_sc, PSA_KEY_USAGE_ENCRYPT, &s->key_s2c);
    }
    memset(key_cs, 0, sizeof(key_cs));
    memset(key_sc, 0, sizeof(key_sc));
    if (err != RV9_IO_OK) return err;

    memcpy(s->iv_c2s, iv_cs, SSH_IV_LEN);
    memcpy(s->iv_s2c, iv_sc, SSH_IV_LEN);
    s->ctr_c2s = ctr_from_iv(iv_cs);
    s->ctr_s2c = ctr_from_iv(iv_sc);

    ssh_packet_begin(s, &p);
    ssh_put_u8(&p, SSH_MSG_NEWKEYS);
    err = ssh_packet_send(s, &p);
    if (err != RV9_IO_OK) return err;

    /*
     * From here out we encrypt; the client keeps sending in the clear
     * until its own NEWKEYS, which is why the two directions have separate
     * switches.
     */
    s->enc_out = true;
    if (s->strict_kex) s->seq_out = 0;

    if (s->want_ext_info) {
        err = send_ext_info(s);
        if (err != RV9_IO_OK) return err;
    }

    err = ssh_packet_read(s);
    if (err != RV9_IO_OK) return err;
    if (s->pay_len == 0 || SSH_PAYLOAD(s)[0] != SSH_MSG_NEWKEYS) {
        return RV9_IO_ERR_IO;
    }

    s->enc_in = true;
    if (s->strict_kex) s->seq_in = 0;

    ESP_LOGI(TAG, "key exchange done%s", s->strict_kex ? " (strict)" : "");
    return RV9_IO_OK;
}
