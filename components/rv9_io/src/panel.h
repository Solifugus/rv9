/*
 * The ST7789, shared by every driver that draws on it.
 *
 * Private to rv9_io: this is hardware, and nothing above a driver has any
 * business knowing the panel exists. See panel.c for why it is not simply
 * owned by the console driver.
 */
#pragma once

#include "rv9/io.h"

#include <stdbool.h>
#include <stdint.h>

/* Bring the panel up if it is not already, and report its geometry. The
   first caller's orientation is the one that takes effect. */
rv9_io_err_t rv9_panel_open(bool landscape, int *w, int *h);

void rv9_panel_size(int *w, int *h);

/* Half-open, like everything else here: x0..x1-1 by y0..y1-1. */
void rv9_panel_blit(int x0, int y0, int x1, int y1, const uint16_t *px);

void     rv9_panel_backlight(uint32_t percent);
uint32_t rv9_panel_backlight_get(void);
