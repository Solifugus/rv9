#pragma once

#include "rv9/module.h"

/*
 * Is `image`, `len` bytes through its signature block, signed by `pub`
 * (65 bytes, an uncompressed P-256 point)? 0 if so, -2 if the signature
 * does not match, -1 if it could not be checked. `out_us` may be NULL.
 * Waits; the work is done in a task of its own (sig.c says why).
 */
int rv9_sig_verify(const void *image, uint32_t len, const uint8_t pub[65],
                   uint32_t *out_us);

/* SHA-256 of `len` bytes, as `sha256sum` gives it. 0, or -1. Waits; done in
   a task of its own. */
int rv9_sha256(const void *data, uint32_t len, uint8_t out[32]);

/* The same, a piece at a time: begin, update as often as there is more,
   end, which frees it whether or not `out` is given. On the caller's
   stack, and not in a task: a hash is shallow. */
typedef struct rv9_sha256_ctx rv9_sha256_ctx_t;
rv9_sha256_ctx_t *rv9_sha256_begin(void);
int rv9_sha256_update(rv9_sha256_ctx_t *c, const void *data, uint32_t len);
int rv9_sha256_end(rv9_sha256_ctx_t *c, uint8_t out[32]);
