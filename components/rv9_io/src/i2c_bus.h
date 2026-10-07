/*
 * The two-wire bus, shared.
 *
 * Two drivers want it on the P4: `i2c` serves whatever is wired to it by
 * address under /i2c0, and the touch controller sits on the same two pins.
 * Only one of them can create the bus and which attaches first is decided
 * by the order descriptors happen to be in -- so neither owns it and this
 * does, which is the arrangement panel.h already makes for the SPI bus the
 * display and the card share on the C5.
 *
 * Private to rv9_io. Nothing above a driver knows there is a bus.
 */
#pragma once

#include "rv9/io.h"

#include "driver/i2c_master.h"

/*
 * Bring the bus up if it is not already, and hand back the handle.
 * Idempotent: a second caller gets the same bus.
 *
 * The pins are the board's and come from rv9_i2c_bus_defaults(); they are
 * not arguments, because two callers passing different ones would be two
 * answers to a question with one. The speed of the first caller wins, and
 * a later caller asking for a different one is told in the log rather than
 * refused -- the bus runs at one speed whatever anybody wanted.
 */
rv9_io_err_t rv9_i2c_bus_claim(uint32_t khz, i2c_master_bus_handle_t *out);

/* What this board wired it to, for a caller with nothing to say about it. */
void rv9_i2c_bus_defaults(uint32_t *sda, uint32_t *scl, uint32_t *khz);
