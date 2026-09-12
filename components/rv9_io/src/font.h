/*
 * The console font, shared by everything that draws characters.
 *
 * It was the console's private business until the window device needed to
 * label a chart. One font, one declaration -- the alternative was a second
 * copy of the geometry constants beside a second extern, and two copies of
 * a number like "the baseline is at row 15" is how a renderer ends up
 * disagreeing with itself about where text sits.
 */
#pragma once

#include <stdint.h>

#define RV9_GLYPH_W      10
#define RV9_GLYPH_H      20
#define RV9_GLYPH_STRIDE ((RV9_GLYPH_W + 1) / 2)   /* two 4-bit pixels a byte */

#define RV9_FONT_FIRST   32
#define RV9_FONT_LAST    126

/*
 * Where the baseline falls in the cell, measured from the top.
 *
 * Found by looking: H and x stop at row 14, g and p descend to 17. The
 * console never needed this -- it puts a glyph in a cell and the cells line
 * up by construction -- but SVG positions text by its baseline, so the
 * number has to be written down somewhere.
 */
#define RV9_FONT_BASELINE 15

extern const unsigned char rv9_font[RV9_FONT_LAST - RV9_FONT_FIRST + 1]
                                   [RV9_GLYPH_H][RV9_GLYPH_STRIDE];

/* Coverage, 0..15, of one pixel of one glyph. Anything out of range is
   blank, so a caller may sample freely around the edges. */
static inline int rv9_glyph_cov(int ch, int gx, int gy)
{
    if (ch < RV9_FONT_FIRST || ch > RV9_FONT_LAST) return 0;
    if (gx < 0 || gx >= RV9_GLYPH_W) return 0;
    if (gy < 0 || gy >= RV9_GLYPH_H) return 0;

    unsigned char b = rv9_font[ch - RV9_FONT_FIRST][gy][gx >> 1];
    return (gx & 1) ? (b & 0x0F) : (b >> 4);
}
