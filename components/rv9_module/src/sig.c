/*
 * Checking a module's signature.
 *
 * ECDSA P-256 over SHA-256 (rv9_mod_sig_t in rv9/module.h), through PSA,
 * which the firmware already carries for SSH. The message is the module up
 * to its signature block with the crc32 field taken as zero, hashed in
 * three pieces so that nothing is copied.
 *
 * WHERE IT RUNS
 *
 * Not on the caller's stack. A module is read off the card inside whoever
 * is starting it, on their stack -- the shell's is 3.5 KB -- and an ECDSA
 * verify is arithmetic on 256-bit numbers whose depth is mbedTLS's
 * business, not ours. Nor in the caller's turn: on a C3 it is done in
 * software, and every RV-9 thread shares one host task. So it is done in a
 * background host task of its own, below RV-9's, with a stack sized for
 * it, and the caller waits on one word as drv_camera.c's reader does. The
 * task says how much of its stack it used, so the size is a measurement.
 */
#include "sig.h"

#include "rv9/kal.h"

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "rv9-sig";

/* Measured 1,232 bytes on the C5, checking a P-256 signature through PSA;
   the rest is room for a different mbedTLS. */
#define SIG_STACK 3072

enum { QUEUED, DONE };

typedef struct {
    const uint8_t *image;
    uint32_t       len;           /* through the block */
    const uint8_t *pub;           /* 65 bytes, uncompressed point */
    volatile int   state;
    int            result;
    uint32_t       stack_used;
} job_t;

static int verify(const job_t *j)
{
    if (psa_crypto_init() != PSA_SUCCESS) return -1;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)j->image;
    const rv9_mod_sig_t *b = (const rv9_mod_sig_t *)(j->image + j->len - sizeof(*b));

    rv9_mod_header_t head;
    memcpy(&head, h, sizeof(head));
    head.crc32 = 0;

    uint8_t hash[32];
    size_t hn = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&op, (const uint8_t *)&head, sizeof(head)) != PSA_SUCCESS ||
        psa_hash_update(&op, j->image + sizeof(head),
                        j->len - sizeof(head) - sizeof(*b)) != PSA_SUCCESS ||
        psa_hash_finish(&op, hash, sizeof(hash), &hn) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        return -1;
    }

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    psa_status_t st = psa_import_key(&a, j->pub, 65, &id);
    psa_reset_key_attributes(&a);
    if (st != PSA_SUCCESS) return -1;

    st = psa_verify_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hn,
                         b->sig, sizeof(b->sig));
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? 0 : -2;
}

static void worker(void *arg)
{
    job_t *j = (job_t *)arg;
    j->result = verify(j);

    size_t size = 0, unused = 0;
    /* A host task's size is not kept, only what it never touched. */
    if (rv9_task_stack(rv9_task_self(), &size, &unused) == RV9_OK) {
        j->stack_used = (uint32_t)((size ? size : SIG_STACK) - unused);
    }
    j->state = DONE;            /* the last thing it touches */
    rv9_task_delete(NULL);
}

int rv9_sig_verify(const void *image, uint32_t len, const uint8_t pub[65],
                   uint32_t *out_us)
{
    job_t j = { .image = image, .len = len, .pub = pub, .state = QUEUED };
    int64_t t0 = rv9_time_us();

    if (rv9_task_create_background(worker, "rv9-sig", SIG_STACK, &j) != RV9_OK) {
        ESP_LOGE(TAG, "no task to check a signature in");
        return -1;
    }
    while (j.state != DONE) rv9_task_delay_ms(2);

    if (out_us) *out_us = (uint32_t)(rv9_time_us() - t0);
    ESP_LOGD(TAG, "checked in %lu us; %lu of %d bytes of stack",
             (unsigned long)(rv9_time_us() - t0),
             (unsigned long)j.stack_used, SIG_STACK);
    return j.result;
}

/* A file's SHA-256, for /f0/pins: in a task of its own, as a verify is,
   since it is asked for on the stack of whoever is starting a module. */
typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint8_t       *out;
    volatile int   state;
    int            result;
} hash_job_t;

static void hash_worker(void *arg)
{
    hash_job_t *j = (hash_job_t *)arg;
    size_t n = 0;
    j->result = psa_crypto_init() == PSA_SUCCESS &&
                psa_hash_compute(PSA_ALG_SHA_256, j->data, j->len, j->out, 32, &n) == PSA_SUCCESS &&
                n == 32 ? 0 : -1;
    j->state = DONE;
    rv9_task_delete(NULL);
}

int rv9_sha256(const void *data, uint32_t len, uint8_t out[32])
{
    hash_job_t j = { .data = data, .len = len, .out = out, .state = QUEUED };
    if (rv9_task_create_background(hash_worker, "rv9-sha", SIG_STACK, &j) != RV9_OK) {
        return -1;
    }
    while (j.state != DONE) rv9_task_delay_ms(1);
    return j.result;
}

/* A file's SHA-256 a piece at a time, for one too big to hold: on the
   caller's stack, as a hash is shallow and each piece is short. */
struct rv9_sha256_ctx { psa_hash_operation_t op; };

rv9_sha256_ctx_t *rv9_sha256_begin(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) return NULL;
    rv9_sha256_ctx_t *c = rv9_alloc(sizeof(*c));
    if (c == NULL) return NULL;
    c->op = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&c->op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        rv9_free(c);
        return NULL;
    }
    return c;
}

int rv9_sha256_update(rv9_sha256_ctx_t *c, const void *data, uint32_t len)
{
    return psa_hash_update(&c->op, data, len) == PSA_SUCCESS ? 0 : -1;
}

int rv9_sha256_end(rv9_sha256_ctx_t *c, uint8_t out[32])
{
    size_t n = 0;
    int r = out != NULL && psa_hash_finish(&c->op, out, 32, &n) == PSA_SUCCESS &&
            n == 32 ? 0 : -1;
    if (r != 0) psa_hash_abort(&c->op);
    rv9_free(c);
    return r;
}

/*
 * This machine's identity (rv9/module.h): who it is to its peers, for `on`
 * and execd. The key pair is rv9_ssh's host key; this keeps only the means
 * to sign with it and the public half. Signing and checking run in a task
 * of their own, as a module's signature is checked above, and for the same
 * reason: they are asked for on the stack of a process opening a path.
 */
static rv9_ident_sign_fn s_sign;
static uint8_t           s_pub[65];

void rv9_ident_set(rv9_ident_sign_fn sign, const uint8_t pub[65])
{
    memcpy(s_pub, pub, sizeof(s_pub));
    s_sign = sign;
}

int rv9_ident_id(const uint8_t pub[65], uint8_t id[16])
{
    uint8_t h[32];
    if (rv9_sha256(pub, 65, h) != 0) return -1;
    memcpy(id, h, 16);
    return 0;
}

bool rv9_ident_get(uint8_t pub[65], uint8_t id[16])
{
    if (s_sign == NULL) return false;
    if (pub) memcpy(pub, s_pub, sizeof(s_pub));
    return id == NULL || rv9_ident_id(s_pub, id) == 0;
}

typedef struct {
    const uint8_t *msg;
    uint32_t       len;
    uint8_t       *sig;           /* out for a signing, in for a check */
    const uint8_t *pub;           /* NULL: sign */
    volatile int   state;
    int            result;
} ident_job_t;

static void ident_worker(void *arg)
{
    ident_job_t *j = (ident_job_t *)arg;
    if (j->pub == NULL) {
        j->result = s_sign(j->msg, j->len, j->sig);
    } else if (psa_crypto_init() != PSA_SUCCESS) {
        j->result = -1;
    } else {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&a, 256);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_MESSAGE);
        psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_key_id_t id = 0;
        psa_status_t st = psa_import_key(&a, j->pub, 65, &id);
        psa_reset_key_attributes(&a);
        if (st != PSA_SUCCESS) {
            j->result = -1;
        } else {
            st = psa_verify_message(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), j->msg, j->len,
                                    j->sig, 64);
            psa_destroy_key(id);
            j->result = st == PSA_SUCCESS ? 0 : -2;
        }
    }
    j->state = DONE;
    rv9_task_delete(NULL);
}

static int ident_job(ident_job_t *j)
{
    if (rv9_task_create_background(ident_worker, "rv9-ident", SIG_STACK, j) != RV9_OK) {
        ESP_LOGE(TAG, "no task to sign or check in");
        return -1;
    }
    while (j->state != DONE) rv9_task_delay_ms(2);
    return j->result;
}

int rv9_ident_sign(const uint8_t *msg, uint32_t len, uint8_t sig[64])
{
    if (s_sign == NULL) return -1;
    ident_job_t j = { .msg = msg, .len = len, .sig = sig, .state = QUEUED };
    return ident_job(&j);
}

int rv9_ident_verify(const uint8_t *msg, uint32_t len, const uint8_t sig[64],
                     const uint8_t pub[65])
{
    ident_job_t j = { .msg = msg, .len = len, .sig = (uint8_t *)sig, .pub = pub,
                      .state = QUEUED };
    return ident_job(&j);
}
