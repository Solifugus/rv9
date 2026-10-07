/*
 * The ST7789, shared by every driver that draws on it.
 *
 * Private to rv9_io: this is hardware, and nothing above a driver has any
 * business knowing the panel exists. See panel.c for why it is not simply
 * owned by the console driver.
 */
#pragma once

#include "rv9/io.h"

#include "driver/spi_master.h"

#include <stdbool.h>
#include <stdint.h>

/* Bring the panel up if it is not already, and report its geometry. The
   first caller's orientation is the one that takes effect. */
rv9_io_err_t rv9_panel_open(bool landscape, int *w, int *h);

/*
 * The SPI bus the panel and the microSD card share.
 *
 * One bus, two chip selects, and either device may be attached first --
 * the order descriptors happen to be in decides it. So whoever gets there
 * first brings the bus up, and this is that call: idempotent, and
 * configured for both (the card needs a data-in line the display never
 * uses). The SPI driver serialises transactions between the two devices.
 */
#define RV9_SPI_HOST SPI2_HOST

rv9_io_err_t rv9_panel_bus_claim(void);

void rv9_panel_size(int *w, int *h);

/*
 * Does this panel want the two bytes of an RGB565 pixel the other way
 * round from native order?
 *
 * The ST7789 does: it is fed pixels over SPI and takes them big-endian, so
 * everything above works in native order -- blends are arithmetic rather
 * than puzzles -- and the swap happens once, where colours are chosen or
 * on a finished band. A DSI panel does not: its scan-out reads the
 * framebuffer out of memory, so memory order *is* the order, and swapping
 * turns navy into olive and an anti-aliased edge into a shadow.
 *
 * Asked once and remembered, never per pixel.
 */
bool rv9_panel_swaps_bytes(void);

/*
 * How the glass is mounted relative to the board, as mirrors to apply.
 *
 * The panel applies these to itself at init -- on the P4 through MADCTL,
 * because the panel there is fitted upside down. A touch controller on the
 * same glass reports in the glass's orientation and has to apply the same
 * ones, or a finger lands where nothing is drawn.
 *
 * Written down here once, for the reason rv9_panel_physical gives: a
 * second place that must agree is a second chance to disagree.
 */
void rv9_panel_mounting(bool *mirror_x, bool *mirror_y, bool *swap_xy);

/*
 * The glass, in micrometres, rotated the same way the pixels are.
 *
 * Here rather than in a descriptor option because there is one panel and the
 * rotation swap already happens here -- `opt2 rotation: must agree with
 * desc_term` is a hazard this board already carries once, and a second
 * "must agree" number would be a second chance to disagree. A different board
 * is a different panel.c, which is true of PANEL_W and PANEL_H already.
 *
 * `kind` is RV9_PHYS_*. This panel is FIXED; a projector would not be.
 */
void rv9_panel_physical(uint32_t *w_um, uint32_t *h_um, uint8_t *kind);

/*
 * Take the glass, and find out whether somebody else had it.
 *
 * There is one panel and no framebuffer, so the only possible model is
 * that whoever painted last is what you see. A device that repaints only
 * what it believes has changed -- the console does -- would otherwise
 * leave the previous owner's picture showing everywhere it thought was
 * still good.
 *
 * Returns true when ownership actually changed, which is that device's cue
 * to repaint all of itself rather than just the parts it knows are dirty.
 */
bool rv9_panel_take(const void *owner);

/* Who is being shown, without taking it. */
bool rv9_panel_is_owner(const void *owner);

/* Half-open, like everything else here: x0..x1-1 by y0..y1-1. */
void rv9_panel_blit(int x0, int y0, int x1, int y1, const uint16_t *px);

void     rv9_panel_backlight(uint32_t percent);
uint32_t rv9_panel_backlight_get(void);
