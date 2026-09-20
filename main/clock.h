/*
 * Wall-clock time, acquired from the network because this board has no
 * clock of its own. See clock.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Begin asking. Safe to call more than once; needs the network up. */
void rv9_clock_start(void);

/*
 * Seconds since the epoch, UTC, and how long ago the network last
 * answered. Returns false when the time has never been set -- in which
 * case both outputs are zero and must not be believed.
 */
bool rv9_clock_read(uint32_t *out_epoch, uint32_t *out_age_s);
