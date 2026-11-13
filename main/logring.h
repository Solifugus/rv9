/*
 * The last few kilobytes of the log, kept in RAM so they can be read over
 * the network instead of off a cable. See logring.c.
 */
#pragma once

#include <stdint.h>

/* What ESP-IDF's panic handler was told, kept across the reset. */
typedef struct {
    uint32_t valid;
    char     reason[48];
    uint32_t core, pc, ra, sp, cause, tval;
} rv9_logring_panic_t;

/* Start keeping. Chains to whatever was printing before, so the serial
   port still gets every line. */
void rv9_logring_start(void);

/* A window, oldest first, from byte `from` of what is currently held.
   Returns how many bytes were written to `out`. */
uint32_t rv9_logring_read(uint32_t from, char *out, uint32_t cap);

/* How many bytes are held right now. */
uint32_t rv9_logring_held(void);

/*
 * Send anything that was logged from real-time context and held back from
 * the port. Ordinary log lines do this themselves; this is for the quiet
 * stretches, where otherwise a held line would wait for the next thing to
 * happen. Safe to call from anywhere that is not itself real-time.
 */
void rv9_logring_flush(void);

/*
 * The previous run's log, kept across the reset (design section 82): the
 * bytes, oldest first, and *len of them, or NULL when there was none (a
 * power-on). `panic` gets what its panic said, valid 0 if it did not
 * panic. Freed by rv9_logring_prev_free once it has been kept elsewhere.
 */
const char *rv9_logring_prev(uint32_t *len, rv9_logring_panic_t *panic);
void        rv9_logring_prev_free(void);

/* How long this run has been up, kept with the box so that the next boot
   knows how long a run that died had lasted; and the last run's figure. */
void        rv9_logring_alive(uint32_t up_s);
uint32_t    rv9_logring_prev_alive(void);
