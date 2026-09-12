/*
 * Anti-aliased polygon coverage into a band of the panel.
 *
 * Coordinates are 8.8 fixed point in panel pixels. Fixed rather than
 * floating because this chip is RV32IMAC: every float would be a library
 * call, in the innermost loop of the whole renderer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int       y0;      /* first panel row this band covers */
    int       w, rows;
    uint16_t *px;      /* w * rows, RGB565 native order while drawing */
    uint16_t *cov;     /* w, scratch coverage for one row */
} rband_t;

#define RV9_FIX(n)  ((int32_t)((n) * 256))

void rv9_raster_clear(rband_t *b, uint16_t colour);

void rv9_raster_fill(rband_t *b, const int32_t *pts, int n, bool evenodd,
                     uint16_t colour);

void rv9_raster_stroke(rband_t *b, const int32_t *pts, int n, bool closed,
                       int32_t width, uint16_t colour);

/* Byte-swap the band into what the ST7789 expects, once, on the way out. */
void rv9_raster_to_panel(rband_t *b);
