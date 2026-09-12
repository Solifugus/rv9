/*
 * Public key authentication.
 *
 * The client proves it holds the private half of a key we already trust,
 * by signing something only this session could have asked for. Two things
 * have to be true and they are independent: the key must be *authorized*,
 * and the signature must be *valid*. Checking one and assuming the other
 * is the classic way to write an SSH server that lets anybody in.
 *
 * Authorization is a byte comparison against `/f0/authkeys`, in the same
 * format as an ordinary authorized_keys file, so the line you already have
 * is the line that goes here. Nothing parses the key to decide that --
 * only to verify with it.
 *
 * Ed25519 is not among the types handled, and not by choice: mbedTLS as
 * ESP-IDF ships it has no EdDSA at all. Since ssh-keygen has defaulted to
 * Ed25519 for years, that is the one thing about this worth knowing in
 * advance.
 */
#include "ssh.h"

#include "rv9/kal.h"

#include "esp_log.h"

static const char *TAG = "rv9-ssh";

#define AUTHKEYS_PATH "/f0/authkeys"
#define AUTHKEYS_MAX  2048
#define KEYBLOB_MAX   600     /* an RSA-4096 blob, with room over */

/* ------------------------------------------------------------------ */
/* base64                                                              */
/* ------------------------------------------------------------------ */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Returns bytes written, or 0 on anything malformed. */
static size_t b64_decode(const char *in, size_t in_len, uint8_t *out,
                         size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;

    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=') break;

        int v = b64_value(in[i]);
        if (v < 0) return 0;

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return 0;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* The authorized keys file                                            */
/* ------------------------------------------------------------------ */

/*
 * Is this exact key blob in the file?
 *
 * Compared as bytes, which is both the simplest thing and the strictest:
 * two keys are the same key only if they encode identically, and there is
 * no parser in the path to disagree with about what a key means.
 *
 * The file is read per authentication attempt rather than cached, so
 * revoking a key is deleting a line and takes effect on the next login,
 * with nothing to restart.
 */
bool ssh_authkey_allowed(const uint8_t *blob, size_t blob_len)
{
    if (blob == NULL || blob_len == 0) return false;

    rv9_path_t *p = NULL;
    if (rv9_io_open_detached(AUTHKEYS_PATH, RV9_MODE_READ, &p) != RV9_IO_OK) {
        return false;
    }

    char *text = rv9_calloc(1, AUTHKEYS_MAX + 1);
    uint8_t *key = rv9_calloc(1, KEYBLOB_MAX);
    size_t total = 0;
    bool found = false;

    if (text == NULL || key == NULL) goto out;

    for (;;) {
        size_t got = 0;
        rv9_io_err_t err = rv9_io_read_path(p, text + total,
                                            AUTHKEYS_MAX - total, &got);
        if (err != RV9_IO_OK || got == 0) break;
        total += got;
        if (total >= AUTHKEYS_MAX) break;
    }
    text[total] = '\0';

    /* Each line is "<type> <base64> [comment]". The type is ignored: the
       blob carries its own, and that is the one that must match. */
    size_t i = 0;
    while (i < total && !found) {
        size_t end = i;
        while (end < total && text[end] != '\n' && text[end] != '\r') end++;

        size_t a = i;
        while (a < end && text[a] == ' ') a++;

        size_t f = a;                                  /* first field */
        while (f < end && text[f] != ' ') f++;
        while (f < end && text[f] == ' ') f++;

        size_t g = f;                                  /* the base64 */
        while (g < end && text[g] != ' ') g++;

        if (g > f && text[a] != '#') {
            size_t n = b64_decode(text + f, g - f, key, KEYBLOB_MAX);
            if (n == blob_len && memcmp(key, blob, n) == 0) found = true;
        }

        i = end;
        while (i < total && (text[i] == '\n' || text[i] == '\r')) i++;
    }

out:
    rv9_free(text);
    rv9_free(key);
    rv9_io_close_path(p);
    return found;
}

/* ------------------------------------------------------------------ */
/* Verifying a signature                                               */
/* ------------------------------------------------------------------ */

/*
 * DER, for RSA only.
 *
 * PSA imports an RSA public key as a DER RSAPublicKey -- SEQUENCE of two
 * INTEGERs -- and SSH supplies two mpints. The happy accident is that an
 * SSH mpint and a DER INTEGER have the same rules: big-endian, minimal
 * length, a leading zero when the top bit is set. So the bytes carry
 * across unchanged and only the wrapping has to be built.
 */
static size_t der_len(uint8_t *out, size_t len)
{
    if (len < 0x80) { out[0] = (uint8_t)len; return 1; }
    if (len < 0x100) { out[0] = 0x81; out[1] = (uint8_t)len; return 2; }
    out[0] = 0x82;
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    return 3;
}

static size_t der_integer(uint8_t *out, size_t cap, const uint8_t *v, size_t n)
{
    uint8_t hdr[4];
    hdr[0] = 0x02;
    size_t hl = 1 + der_len(hdr + 1, n);

    if (hl + n > cap) return 0;
    memcpy(out, hdr, hl);
    memcpy(out + hl, v, n);
    return hl + n;
}

static size_t rsa_to_der(const uint8_t *n, size_t n_len,
                         const uint8_t *e, size_t e_len,
                         uint8_t *out, size_t cap)
{
    uint8_t body[KEYBLOB_MAX];
    size_t b = 0;

    size_t got = der_integer(body + b, sizeof(body) - b, n, n_len);
    if (got == 0) return 0;
    b += got;

    got = der_integer(body + b, sizeof(body) - b, e, e_len);
    if (got == 0) return 0;
    b += got;

    uint8_t hdr[4];
    hdr[0] = 0x30;
    size_t hl = 1 + der_len(hdr + 1, b);

    if (hl + b > cap) return 0;
    memcpy(out, hdr, hl);
    memcpy(out + hl, body, b);
    return hl + b;
}

static rv9_io_err_t verify_ecdsa(ssh_buf_t *key, const uint8_t *sig,
                                 size_t sig_len, const uint8_t *data,
                                 size_t data_len)
{
    size_t n = 0;
    const uint8_t *curve = ssh_get_string(key, &n);
    if (key->bad || n != 8 || memcmp(curve, "nistp256", 8) != 0) {
        return RV9_IO_ERR_UNSUPPORTED;
    }

    size_t q_len = 0;
    const uint8_t *q = ssh_get_string(key, &q_len);
    if (key->bad || q_len != SSH_HOSTPUB_LEN || q[0] != 0x04) {
        return RV9_IO_ERR_INVAL;
    }

    /* The signature is two mpints; PSA wants r and s as thirty-two bytes
       each, so the sign bytes come off and short values are padded up. */
    ssh_buf_t sb;
    ssh_buf_load(&sb, (uint8_t *)sig, sig_len);

    uint8_t rs[SSH_SIG_LEN];
    memset(rs, 0, sizeof(rs));

    for (int half = 0; half < 2; half++) {
        size_t len = 0;
        const uint8_t *v = ssh_get_string(&sb, &len);
        if (sb.bad) return RV9_IO_ERR_INVAL;

        while (len > 0 && v[0] == 0) { v++; len--; }
        if (len > 32) return RV9_IO_ERR_INVAL;

        memcpy(rs + half * 32 + (32 - len), v, len);
    }

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t id = 0;
    psa_status_t st = psa_import_key(&a, q, q_len, &id);
    psa_reset_key_attributes(&a);
    if (st != PSA_SUCCESS) return RV9_IO_ERR_INVAL;

    st = psa_verify_message(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                            data, data_len, rs, sizeof(rs));
    psa_destroy_key(id);

    return (st == PSA_SUCCESS) ? RV9_IO_OK : RV9_IO_ERR_MODE;
}

static rv9_io_err_t verify_rsa(ssh_buf_t *key, const char *sig_alg,
                               const uint8_t *sig, size_t sig_len,
                               const uint8_t *data, size_t data_len)
{
    psa_algorithm_t hash;
    if (strcmp(sig_alg, "rsa-sha2-256") == 0)      hash = PSA_ALG_SHA_256;
    else if (strcmp(sig_alg, "rsa-sha2-512") == 0) hash = PSA_ALG_SHA_512;
    else return RV9_IO_ERR_UNSUPPORTED;   /* ssh-rsa is SHA-1; not offered */

    size_t e_len = 0, n_len = 0;
    const uint8_t *e = ssh_get_string(key, &e_len);
    const uint8_t *n = ssh_get_string(key, &n_len);
    if (key->bad) return RV9_IO_ERR_INVAL;

    /* An mpint may carry a leading zero for its sign; DER wants the same
       thing, so this is only a sanity bound, not a conversion. */
    if (n_len == 0 || n_len > KEYBLOB_MAX / 2) return RV9_IO_ERR_INVAL;

    uint8_t *der = rv9_calloc(1, KEYBLOB_MAX);
    if (der == NULL) return RV9_IO_ERR_NOMEM;

    size_t der_n = rsa_to_der(n, n_len, e, e_len, der, KEYBLOB_MAX);
    if (der_n == 0) { rv9_free(der); return RV9_IO_ERR_INVAL; }

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_RSA_PKCS1V15_SIGN(hash));

    psa_key_id_t id = 0;
    psa_status_t st = psa_import_key(&a, der, der_n, &id);
    psa_reset_key_attributes(&a);
    rv9_free(der);

    if (st != PSA_SUCCESS) {
        ESP_LOGW(TAG, "cannot import RSA key: %d", (int)st);
        return RV9_IO_ERR_INVAL;
    }

    st = psa_verify_message(id, PSA_ALG_RSA_PKCS1V15_SIGN(hash),
                            data, data_len, sig, sig_len);
    psa_destroy_key(id);

    return (st == PSA_SUCCESS) ? RV9_IO_OK : RV9_IO_ERR_MODE;
}

/*
 * Verify `sig` over `data` with the public key in `blob`.
 *
 * The key's own type comes from inside the blob, never from what the
 * client claimed alongside it -- those are two different fields and only
 * one of them is covered by the signature.
 */
rv9_io_err_t ssh_pubkey_verify(const uint8_t *blob, size_t blob_len,
                               const char *sig_alg,
                               const uint8_t *sig, size_t sig_len,
                               const uint8_t *data, size_t data_len)
{
    ssh_buf_t key;
    ssh_buf_load(&key, (uint8_t *)blob, blob_len);

    char type[32];
    ssh_get_cstr(&key, type, sizeof(type));
    if (key.bad) return RV9_IO_ERR_INVAL;

    /* The signature blob names its algorithm too, and that one the client
       chose freely; it decides the hash, not the key. */
    ssh_buf_t sb;
    ssh_buf_load(&sb, (uint8_t *)sig, sig_len);

    char alg[32];
    ssh_get_cstr(&sb, alg, sizeof(alg));
    size_t inner_len = 0;
    const uint8_t *inner = ssh_get_string(&sb, &inner_len);
    if (sb.bad) return RV9_IO_ERR_INVAL;

    if (sig_alg != NULL && strcmp(alg, sig_alg) != 0) {
        return RV9_IO_ERR_INVAL;
    }

    if (strcmp(type, "ecdsa-sha2-nistp256") == 0 &&
        strcmp(alg, "ecdsa-sha2-nistp256") == 0) {
        return verify_ecdsa(&key, inner, inner_len, data, data_len);
    }

    if (strcmp(type, "ssh-rsa") == 0) {
        return verify_rsa(&key, alg, inner, inner_len, data, data_len);
    }

    ESP_LOGW(TAG, "key type '%s' is not one this build can verify", type);
    return RV9_IO_ERR_UNSUPPORTED;
}
