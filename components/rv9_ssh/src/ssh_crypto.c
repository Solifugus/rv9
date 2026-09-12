/*
 * The algorithms, the host key, and the password.
 *
 * Everything cryptographic goes through PSA. mbedTLS 4 removed the old
 * mbedtls_ecdh/mbedtls_aes surface in favour of it, which turns out to be
 * a gift for this particular job: PSA hands X25519 keys back as thirty-two
 * raw bytes, which is exactly what SSH puts on the wire, so the key
 * exchange needs no format conversion anywhere.
 *
 * The host key and the password live in NVS, like the WiFi credentials and
 * for the same reason: they are this board's identity and this board's
 * secret, not the source tree's.
 */
#include "ssh.h"

#include "rv9/kal.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "rv9-ssh";

#define NVS_NAMESPACE   "rv9ssh"
#define NVS_KEY_HOST    "hostkey"
#define NVS_KEY_SALT    "pwsalt"
#define NVS_KEY_HASH    "pwhash"

#define SALT_LEN        16
#define PW_ROUNDS       10000

static psa_key_id_t s_hostkey;
static uint8_t      s_hostpub[SSH_HOSTPUB_LEN];
static bool         s_ready;

rv9_io_err_t ssh_crypto_init(void)
{
    /* Idempotent by specification, and something else in the system may
       well have got here first. */
    return (psa_crypto_init() == PSA_SUCCESS) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* The host key                                                        */
/* ------------------------------------------------------------------ */

static psa_status_t import_hostkey(const uint8_t priv[SSH_HOSTPRIV_LEN])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_status_t st = psa_import_key(&a, priv, SSH_HOSTPRIV_LEN, &s_hostkey);
    psa_reset_key_attributes(&a);
    return st;
}

static rv9_io_err_t generate_hostkey(uint8_t priv[SSH_HOSTPRIV_LEN])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t id = 0;
    psa_status_t st = psa_generate_key(&a, &id);
    psa_reset_key_attributes(&a);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "host key generation failed: %d", (int)st);
        return RV9_IO_ERR_IO;
    }

    size_t n = 0;
    st = psa_export_key(id, priv, SSH_HOSTPRIV_LEN, &n);
    psa_destroy_key(id);

    if (st != PSA_SUCCESS || n != SSH_HOSTPRIV_LEN) return RV9_IO_ERR_IO;
    return RV9_IO_OK;
}

/*
 * Load the host key, making one the first time.
 *
 * A host key that changed every boot would make the client's warning about
 * a changed key meaningless, which is the same as having no warning. So it
 * is generated once, on the first connection this board ever serves, and
 * kept.
 */
rv9_io_err_t ssh_hostkey_load(void)
{
    if (s_ready) return RV9_IO_OK;

    uint8_t priv[SSH_HOSTPRIV_LEN];
    bool have = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(priv);
        have = (nvs_get_blob(h, NVS_KEY_HOST, priv, &len) == ESP_OK &&
                len == sizeof(priv));
        nvs_close(h);
    }

    if (!have) {
        ESP_LOGI(TAG, "no host key yet, generating one");
        rv9_io_err_t err = generate_hostkey(priv);
        if (err != RV9_IO_OK) return err;

        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_blob(h, NVS_KEY_HOST, priv, sizeof(priv)) == ESP_OK) {
                nvs_commit(h);
            }
            nvs_close(h);
        } else {
            /* Serve this session anyway: a board that cannot write its key
               is still better than one that refuses to talk. It will just
               have a different identity next boot, and say so. */
            ESP_LOGW(TAG, "cannot store host key; it will change on reboot");
        }
    }

    psa_status_t st = import_hostkey(priv);
    memset(priv, 0, sizeof(priv));
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "host key import failed: %d", (int)st);
        return RV9_IO_ERR_IO;
    }

    size_t n = 0;
    if (psa_export_public_key(s_hostkey, s_hostpub, sizeof(s_hostpub), &n)
            != PSA_SUCCESS || n != SSH_HOSTPUB_LEN) {
        return RV9_IO_ERR_IO;
    }

    s_ready = true;

    char fp[64];
    ssh_hostkey_fingerprint(fp, sizeof(fp));
    ESP_LOGI(TAG, "host key %s", fp);
    return RV9_IO_OK;
}

/*
 * K_S, the public host key blob:
 *
 *   string  "ecdsa-sha2-nistp256"
 *   string  "nistp256"
 *   string  Q            -- 0x04 || X || Y, which is what PSA exports
 */
void ssh_hostkey_blob(ssh_buf_t *s)
{
    uint8_t blob[128];
    ssh_buf_t b;
    ssh_buf_init(&b, blob, sizeof(blob));

    ssh_put_cstr(&b, "ecdsa-sha2-nistp256");
    ssh_put_cstr(&b, "nistp256");
    ssh_put_string(&b, s_hostpub, sizeof(s_hostpub));

    ssh_put_string(s, b.b, b.len);
}

/*
 * The signature blob:
 *
 *   string  "ecdsa-sha2-nistp256"
 *   string  ( mpint r || mpint s )
 *
 * PSA returns r and s as sixty-four raw bytes; SSH wants them as mpints,
 * which is where the leading-zero rule earns its keep.
 *
 * The exchange hash is the *message* here, not the digest -- it gets
 * hashed again with SHA-256 before ECDSA sees it. That reads like a
 * mistake and is not: RFC 5656 says the signature is computed over H using
 * the curve's hash, and every client does the same thing on the way in.
 * Signing H directly produces a signature of exactly the right shape that
 * every client rejects, which is a slow afternoon.
 */
rv9_io_err_t ssh_hostkey_sign(const uint8_t hash[SSH_HASH_LEN], ssh_buf_t *s)
{
    uint8_t raw[SSH_SIG_LEN];
    size_t  n = 0;

    if (psa_sign_message(s_hostkey, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                         hash, SSH_HASH_LEN, raw, sizeof(raw), &n)
            != PSA_SUCCESS || n != SSH_SIG_LEN) {
        return RV9_IO_ERR_IO;
    }

    uint8_t rs[80];
    ssh_buf_t b;
    ssh_buf_init(&b, rs, sizeof(rs));
    ssh_put_mpint(&b, raw, 32);
    ssh_put_mpint(&b, raw + 32, 32);

    uint8_t blob[128];
    ssh_buf_t o;
    ssh_buf_init(&o, blob, sizeof(blob));
    ssh_put_cstr(&o, "ecdsa-sha2-nistp256");
    ssh_put_string(&o, b.b, b.len);

    if (b.bad || o.bad) return RV9_IO_ERR_IO;

    ssh_put_string(s, o.b, o.len);
    return RV9_IO_OK;
}

/* SHA256 of the key blob, base64, the way every client prints it. */
void ssh_hostkey_fingerprint(char *out, size_t cap)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (cap == 0) return;
    out[0] = '\0';
    if (!s_ready && cap > 12) { memcpy(out, "no host key", 12); return; }

    uint8_t blob[128];
    ssh_buf_t b;
    ssh_buf_init(&b, blob, sizeof(blob));
    ssh_put_cstr(&b, "ecdsa-sha2-nistp256");
    ssh_put_cstr(&b, "nistp256");
    ssh_put_string(&b, s_hostpub, sizeof(s_hostpub));

    uint8_t h[SSH_HASH_LEN];
    size_t  hn = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, b.b, b.len, h, sizeof(h), &hn)
            != PSA_SUCCESS) {
        return;
    }

    /* "SHA256:" then unpadded base64, 43 characters. */
    size_t o = 0;
    const char *pre = "SHA256:";
    while (*pre && o + 1 < cap) out[o++] = *pre++;

    for (size_t i = 0; i < hn; i += 3) {
        uint32_t v = (uint32_t)h[i] << 16;
        if (i + 1 < hn) v |= (uint32_t)h[i + 1] << 8;
        if (i + 2 < hn) v |= h[i + 2];

        int chars = (i + 2 < hn) ? 4 : (i + 1 < hn) ? 3 : 2;
        for (int c = 0; c < chars && o + 1 < cap; c++) {
            out[o++] = b64[(v >> (18 - 6 * c)) & 0x3F];
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* The password                                                        */
/* ------------------------------------------------------------------ */

/*
 * Stored as a salted, iterated SHA-256 rather than as itself.
 *
 * Twenty thousand rounds costs a few milliseconds here, because the SHA
 * unit is in hardware, and costs an attacker who has extracted the flash
 * the same factor. It is not a memory-hard function and does not pretend
 * to be; it is the difference between a password that is readable with a
 * flash programmer and one that is not.
 */
static void derive(const char *password, const uint8_t salt[SALT_LEN],
                   uint8_t out[SSH_HASH_LEN])
{
    uint8_t work[SALT_LEN + 64];
    size_t  plen = strlen(password);
    if (plen > 64) plen = 64;

    memcpy(work, salt, SALT_LEN);
    memcpy(work + SALT_LEN, password, plen);

    size_t n = 0;
    psa_hash_compute(PSA_ALG_SHA_256, work, SALT_LEN + plen,
                     out, SSH_HASH_LEN, &n);

    uint8_t round[SSH_HASH_LEN + SALT_LEN];
    memcpy(round + SSH_HASH_LEN, salt, SALT_LEN);

    for (int i = 0; i < PW_ROUNDS; i++) {
        memcpy(round, out, SSH_HASH_LEN);
        psa_hash_compute(PSA_ALG_SHA_256, round, sizeof(round),
                         out, SSH_HASH_LEN, &n);
    }

    memset(work, 0, sizeof(work));
}

bool ssh_password_set(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = SSH_HASH_LEN;
    bool ok = (nvs_get_blob(h, NVS_KEY_HASH, NULL, &len) == ESP_OK &&
               len == SSH_HASH_LEN);
    nvs_close(h);
    return ok;
}

rv9_io_err_t ssh_password_store(const char *password)
{
    if (password == NULL || password[0] == '\0') return RV9_IO_ERR_INVAL;

    uint8_t salt[SALT_LEN];
    if (psa_generate_random(salt, sizeof(salt)) != PSA_SUCCESS) {
        return RV9_IO_ERR_IO;
    }

    uint8_t hash[SSH_HASH_LEN];
    derive(password, salt, hash);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }

    rv9_io_err_t err = RV9_IO_ERR_IO;
    if (nvs_set_blob(h, NVS_KEY_SALT, salt, sizeof(salt)) == ESP_OK &&
        nvs_set_blob(h, NVS_KEY_HASH, hash, sizeof(hash)) == ESP_OK &&
        nvs_commit(h) == ESP_OK) {
        err = RV9_IO_OK;
    }
    nvs_close(h);

    memset(hash, 0, sizeof(hash));
    return err;
}

bool ssh_password_check(const char *password)
{
    uint8_t salt[SALT_LEN], want[SSH_HASH_LEN];

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t slen = sizeof(salt), hlen = sizeof(want);
    bool have = (nvs_get_blob(h, NVS_KEY_SALT, salt, &slen) == ESP_OK &&
                 nvs_get_blob(h, NVS_KEY_HASH, want, &hlen) == ESP_OK &&
                 slen == sizeof(salt) && hlen == sizeof(want));
    nvs_close(h);

    if (!have) return false;

    uint8_t got[SSH_HASH_LEN];
    derive(password, salt, got);

    /* Compare in constant time: an early exit leaks how many bytes of a
       guess were right, which is enough to find the rest one at a time. */
    uint8_t diff = 0;
    for (int i = 0; i < SSH_HASH_LEN; i++) diff |= (uint8_t)(got[i] ^ want[i]);

    memset(got, 0, sizeof(got));
    return diff == 0;
}
