/*
 * The last few kilobytes of the log, kept in RAM so they can be read over
 * the network instead of off a cable. See logring.c.
 */
#pragma once

#include <stdint.h>

/* Start keeping. Chains to whatever was printing before, so the serial
   port still gets every line. */
void rv9_logring_start(void);

/* A window, oldest first, from byte `from` of what is currently held.
   Returns how many bytes were written to `out`. */
uint32_t rv9_logring_read(uint32_t from, char *out, uint32_t cap);

/* How many bytes are held right now. */
uint32_t rv9_logring_held(void);
