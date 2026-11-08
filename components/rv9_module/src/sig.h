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
