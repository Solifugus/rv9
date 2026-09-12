/*
 * A polygon rasteriser, one band at a time.
 *
 * There is no framebuffer. 320x172 at two bytes a pixel is 110 KB and this
 * board has about forty free, so the panel is painted in horizontal bands
 * a few rows deep: fill a band, push it down the SPI bus, move on.
 *
 * That decides the shape of everything above. A renderer with a
 * framebuffer can draw shapes in any order and composite as it goes; a
 * banded one has to be able to ask, for each band, what is in it -- which
 * is why the SVG source is re-read once per band rather than compiled into
 * a display list. Re-reading a few kilobytes of text twenty times costs
 * far less than the memory the display list would have needed, and there
 * is no second representation to keep in step with the first.
 *
 * Coverage is computed with four subsamples per pixel row and exact
 * fractional span ends horizontally. That is cheap and it matters: on a
 * panel this small, an unantialiased diagonal is unmistakably a staircase.
 */
#include "raster.h"

#include <string.h>

#define SUB       4                  /* vertical subsamples per pixel row */
#define SUB_FULL  (256 / SUB)        /* coverage one subsample contributes */

/*
 * Add coverage for one horizontal span, in 8.8 fixed point.
 *
 * The end pixels get the fraction they are actually covered by, which is
 * where the horizontal half of the anti-aliasing comes from -- the
 * vertical half is the subsampling.
 */
static void add_span(uint16_t *cov, int w, int32_t xa, int32_t xb, int amount)
{
    if (xb <= xa) return;

    if (xa < 0) xa = 0;
    if (xb > (int32_t)w << 8) xb = (int32_t)w << 8;
    if (xb <= xa) return;

    int ia = (int)(xa >> 8);
    int ib = (int)((xb - 1) >> 8);

    if (ia == ib) {
        cov[ia] = (uint16_t)(cov[ia] + (amount * (xb - xa) >> 8));
        return;
    }

    cov[ia] = (uint16_t)(cov[ia] + (amount * (256 - (xa & 255)) >> 8));
    for (int i = ia + 1; i < ib; i++) cov[i] = (uint16_t)(cov[i] + amount);
    cov[ib] = (uint16_t)(cov[ib] + (amount * (xb - ((int32_t)ib << 8)) >> 8));
}

typedef struct {
    int32_t x;
    int     dir;
} cross_t;

#define MAX_CROSS 64

/* Where the edges cross this subscanline, left to right. */
static int crossings(const int32_t *pts, int n, int32_t sy, cross_t *out)
{
    int m = 0;

    for (int i = 0; i < n && m < MAX_CROSS; i++) {
        int j = (i + 1) % n;

        int32_t x0 = pts[2 * i], y0 = pts[2 * i + 1];
        int32_t x1 = pts[2 * j], y1 = pts[2 * j + 1];

        if (y0 == y1) continue;

        int dir = 1;
        if (y0 > y1) {
            int32_t t;
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
            dir = -1;
        }
        /* Half-open in y, so a vertex shared by two edges is counted once
           and shapes do not develop pinholes along their seams. */
        if (sy < y0 || sy >= y1) continue;

        int64_t dx = (int64_t)(x1 - x0) * (sy - y0);
        out[m].x = x0 + (int32_t)(dx / (y1 - y0));
        out[m].dir = dir;
        m++;
    }

    for (int i = 1; i < m; i++) {
        cross_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].x > k.x) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return m;
}

/* RGB565, native order. The swap to what the panel wants happens once per
   band, on the way out. */
static uint16_t blend(uint16_t fg, uint16_t bg, int a)
{
    if (a >= 255) return fg;
    if (a <= 0) return bg;

    int fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;

    int r = br  + ((fr  - br)  * a >> 8);
    int g = bgc + ((fgc - bgc) * a >> 8);
    int b = bb  + ((fb  - bb)  * a >> 8);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

void rv9_raster_fill(rband_t *b, const int32_t *pts, int n, bool evenodd,
                     uint16_t colour)
{
    if (n < 3) return;

    /* Skip the whole shape if it is not in this band at all -- which is
       most shapes, most bands, and the reason banding is affordable. */
    int32_t ymin = pts[1], ymax = pts[1];
    for (int i = 1; i < n; i++) {
        if (pts[2 * i + 1] < ymin) ymin = pts[2 * i + 1];
        if (pts[2 * i + 1] > ymax) ymax = pts[2 * i + 1];
    }
    if (ymax <= (int32_t)b->y0 << 8) return;
    if (ymin >= (int32_t)(b->y0 + b->rows) << 8) return;

    cross_t cr[MAX_CROSS];

    for (int row = 0; row < b->rows; row++) {
        int py = b->y0 + row;
        if (((int32_t)(py + 1) << 8) <= ymin) continue;
        if (((int32_t)py << 8) >= ymax) continue;

        memset(b->cov, 0, (size_t)b->w * sizeof(uint16_t));
        bool any = false;

        for (int s = 0; s < SUB; s++) {
            int32_t sy = ((int32_t)py << 8) + (256 * s + 128) / SUB;

            int m = crossings(pts, n, sy, cr);
            if (m < 2) continue;

            if (evenodd) {
                for (int i = 0; i + 1 < m; i += 2) {
                    add_span(b->cov, b->w, cr[i].x, cr[i + 1].x, SUB_FULL);
                    any = true;
                }
            } else {
                int wind = 0;
                for (int i = 0; i + 1 < m; i++) {
                    wind += cr[i].dir;
                    if (wind != 0) {
                        add_span(b->cov, b->w, cr[i].x, cr[i + 1].x, SUB_FULL);
                        any = true;
                    }
                }
            }
        }

        if (!any) continue;

        uint16_t *line = &b->px[(size_t)row * b->w];
        for (int x = 0; x < b->w; x++) {
            int a = b->cov[x];
            if (a == 0) continue;
            if (a > 255) a = 255;
            line[x] = blend(colour, line[x], a);
        }
    }
}

/*
 * Stroke, as a filled quad per segment.
 *
 * Real stroking builds one outline for the whole path and handles joins
 * and caps; this draws each segment separately and lets them overlap. At
 * the widths a 320-pixel panel can show, the difference is a slightly
 * blunt corner, and blending twice toward the same opaque colour lands on
 * that colour rather than darkening -- so the overlap does not show.
 */
void rv9_raster_stroke(rband_t *b, const int32_t *pts, int n, bool closed,
                       int32_t width, uint16_t colour)
{
    if (n < 2 || width <= 0) return;

    int32_t half = width / 2;
    if (half < 1) half = 1;

    int segs = closed ? n : n - 1;

    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % n;

        int32_t x0 = pts[2 * i], y0 = pts[2 * i + 1];
        int32_t x1 = pts[2 * j], y1 = pts[2 * j + 1];

        int32_t dx = x1 - x0, dy = y1 - y0;
        if (dx == 0 && dy == 0) continue;

        /* Perpendicular, scaled to half the stroke width. Lengths are in
           8.8 fixed, so the square root runs on whole pixels to keep the
           intermediate from overflowing. */
        int32_t len = 0;
        {
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            int64_t r = 0, bit = (int64_t)1 << 40;
            while (bit > d2) bit >>= 2;
            while (bit) {
                if (d2 >= r + bit) { d2 -= r + bit; r = (r >> 1) + bit; }
                else r >>= 1;
                bit >>= 2;
            }
            len = (int32_t)r;
        }
        if (len == 0) continue;

        int32_t px = (int32_t)(-(int64_t)dy * half / len);
        int32_t py = (int32_t)((int64_t)dx * half / len);

        int32_t quad[8] = {
            x0 + px, y0 + py,
            x1 + px, y1 + py,
            x1 - px, y1 - py,
            x0 - px, y0 - py,
        };
        rv9_raster_fill(b, quad, 4, false, colour);
    }
}

void rv9_raster_clear(rband_t *b, uint16_t colour)
{
    size_t n = (size_t)b->w * b->rows;
    for (size_t i = 0; i < n; i++) b->px[i] = colour;
}

/* The panel takes its pixels big-endian; everything above works in native
   order so the blends are arithmetic rather than puzzles. */
void rv9_raster_to_panel(rband_t *b)
{
    size_t n = (size_t)b->w * b->rows;
    for (size_t i = 0; i < n; i++) {
        uint16_t v = b->px[i];
        b->px[i] = (uint16_t)((v >> 8) | (v << 8));
    }
}
