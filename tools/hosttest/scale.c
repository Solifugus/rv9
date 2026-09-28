/*
 * scale -- what the SVG window costs at a bigger screen.
 *
 *   scale <w> <h> <file.svg> [repeats]
 *
 * The renderer was written for a 172x320 SPI panel. The P4 board that is
 * coming has a 1024x600 MIPI-DSI panel, and the question worth answering
 * before the hardware arrives is not "does it work" -- the code has no
 * dimension it cannot take -- but *what does it cost*, because two of its
 * costs scale differently:
 *
 *   - the buffers scale with WIDTH:  band = w * BAND_ROWS * 2, cov = w * 2
 *   - the parse count scales with HEIGHT: the source is re-read once per
 *     band of BAND_ROWS rows, which is what lets a document be larger than
 *     the RAM available to draw it
 *
 * So a taller screen does not cost more memory, it costs more *parsing*, and
 * a wider one is the reverse. Measuring both separately is the point of
 * taking w and h rather than a panel name.
 *
 * Deliberately not part of run.sh's assertions: there is no right answer
 * here to pass or fail against, only numbers to read.
 */
#include "drv_svgwin.c"      /* statics and all: this is a test of the inside */
#include <stdio.h>
#include <time.h>

static int      PW = 320, PH = 172;
static uint16_t *screen;
static int      ROT = 1;

rv9_io_err_t rv9_panel_open(bool l, int *w, int *h)
{ (void)l; if (w) *w = PW; if (h) *h = PH; return RV9_IO_OK; }
void rv9_panel_size(int *w, int *h){ if (w) *w = PW; if (h) *h = PH; }
void rv9_panel_backlight(uint32_t p){ (void)p; }
uint32_t rv9_panel_backlight_get(void){ return 100; }
bool rv9_panel_take(const void *o){ (void)o; return false; }

/* Counted rather than drawn: the blit is the panel's cost, not the
   renderer's, and on the C5 it is an SPI transfer this host does not have. */
static long blits, blit_px;

void rv9_panel_blit(int x0, int y0, int x1, int y1, const uint16_t *px)
{
    blits++;
    blit_px += (long)(x1 - x0) * (y1 - y0);
    for (int y = y0; y < y1 && y < PH; y++) {
        for (int x = x0; x < x1 && x < PW; x++) {
            screen[y * PW + x] = px[(y - y0) * (x1 - x0) + (x - x0)];
        }
    }
}

/* RGB565 back to bytes. Stored byte-swapped by rv9_raster_to_panel, which
   is the panel's wire order rather than a colour decision. */
static void rgb(uint16_t v, int *r, int *g, int *b)
{
    uint16_t n = (uint16_t)((v >> 8) | (v << 8));
    *r = ((n >> 11) & 0x1F) * 255 / 31;
    *g = ((n >> 5)  & 0x3F) * 255 / 63;
    *b = ( n        & 0x1F) * 255 / 31;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1.0e6;
}

/*
 * Render once at the given size and hand back the buffer.
 *
 * Used twice by the comparison below, which is the only correctness check
 * available here without hardcoding what each picture should look like: the
 * same document at two sizes must agree at the same *relative* positions.
 * Nothing says what the colours are; it says the renderer scales without
 * changing its mind.
 */
static uint16_t *render_at(int w, int h, const char *src, size_t n, double *ms)
{
    PW = w; PH = h;
    free(screen);
    screen = calloc((size_t)PW * PH, sizeof(uint16_t));
    if (screen == NULL) return NULL;

    rv9_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.opt[OPT_ROTATE] = ROT;
    dev.opt[OPT_BG] = 0x0000;
    if (svgwin_init(&dev) != RV9_IO_OK) return NULL;
    if (svgwin_open(&dev, 3) != RV9_IO_OK) return NULL;

    double t0 = now_ms();
    size_t done = 0;
    blits = blit_px = 0;
    svgwin_write(&dev, (char *)src, n, &done);
    if (ms) *ms = now_ms() - t0;

    uint16_t *out = screen;
    screen = NULL;
    return out;
}

/* Agreement between two renders at the same relative positions. A boundary
   moves by a fraction of a pixel when the scale changes, so samples are
   taken away from edges and a near-match is a match. */
static int compare(uint16_t *a, int aw, int ah, uint16_t *b, int bw, int bh)
{
    int checked = 0, differ = 0, edge = 0;

    for (int gy = 1; gy < 16; gy++) {
        for (int gx = 1; gx < 16; gx++) {
            double fx = gx / 16.0, fy = gy / 16.0;
            uint16_t va = a[(int)(fy * ah) * aw + (int)(fx * aw)];
            uint16_t vb = b[(int)(fy * bh) * bw + (int)(fx * bw)];

            int ar, ag, ab, br, bg, bb;
            rgb(va, &ar, &ag, &ab);
            rgb(vb, &br, &bg, &bb);
            checked++;
            if (abs(ar - br) <= 40 && abs(ag - bg) <= 40 && abs(ab - bb) <= 40) {
                continue;                       /* agrees outright */
            }

            /*
             * Not the same colour at the same relative point -- but is it
             * the same colour a pixel or two away in the larger render?
             *
             * A shape boundary lands on a different fraction of a pixel at a
             * different scale, so an anti-aliased edge blends differently and
             * a sample that falls on one is *supposed* to disagree. What
             * would be a real fault is a colour that appears nowhere nearby.
             * This separates the two instead of guessing which it is.
             */
            int bx = (int)(fx * bw), by = (int)(fy * bh);
            bool nearby = false;
            int reach = 2 + bw / aw;            /* one reference pixel, in b */

            for (int dy = -reach; dy <= reach && !nearby; dy++) {
                for (int dx = -reach; dx <= reach && !nearby; dx++) {
                    int px = bx + dx, py = by + dy;
                    if (px < 0 || py < 0 || px >= bw || py >= bh) continue;
                    int nr, ng, nb;
                    rgb(b[py * bw + px], &nr, &ng, &nb);
                    if (abs(ar - nr) <= 40 && abs(ag - ng) <= 40 &&
                        abs(ab - nb) <= 40) {
                        nearby = true;
                    }
                }
            }

            if (nearby) { edge++; continue; }

            differ++;
            if (differ <= 4) {
                printf("        WRONG at %.2f,%.2f: %d,%d,%d vs %d,%d,%d "
                       "(not found within %d px)\n",
                       fx, fy, ar, ag, ab, br, bg, bb, reach);
            }
        }
    }
    printf("        %d of %d agree (%d of those only after looking within a "
           "reference pixel -- anti-aliased edges), %d wrong\n",
           checked - differ, checked, edge, differ);
    return differ;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        puts("usage: scale <w> <h> <file.svg> [repeats]");
        puts("       scale compare <w> <h> <file.svg>");
        return 2;
    }

    if (strcmp(argv[1], "compare") == 0) {
        if (argc < 5) { puts("usage: scale compare <w> <h> <file.svg>"); return 2; }
        int w = atoi(argv[2]), h = atoi(argv[3]);

        FILE *cf = fopen(argv[4], "rb");
        if (!cf) { puts("no svg"); return 1; }
        static char cbuf[SRC_MAX * 4];
        size_t cn = fread(cbuf, 1, sizeof cbuf, cf);
        fclose(cf);

        double ms_ref = 0, ms_big = 0;
        uint16_t *ref = render_at(320, 172, cbuf, cn, &ms_ref);
        uint16_t *big = render_at(w, h, cbuf, cn, &ms_big);
        if (ref == NULL || big == NULL) { puts("render failed"); return 1; }

        printf("  %s: 320x172 in %.2f ms, %dx%d in %.2f ms\n",
               argv[4], ms_ref, w, h, ms_big);
        int differ = compare(ref, 320, 172, big, w, h);
        return differ ? 1 : 0;
    }
    PW = atoi(argv[1]);
    PH = atoi(argv[2]);
    int reps = (argc > 4) ? atoi(argv[4]) : 1;
    if (reps < 1) reps = 1;

    screen = calloc((size_t)PW * PH, sizeof(uint16_t));
    if (screen == NULL) { puts("no screen"); return 1; }

    FILE *f = fopen(argv[3], "rb");
    if (!f) { puts("no svg"); return 1; }
    static char buf[SRC_MAX * 4];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    rv9_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.opt[OPT_ROTATE] = ROT;
    dev.opt[OPT_BG] = 0x0000;

    if (svgwin_init(&dev) != RV9_IO_OK) { puts("init failed"); return 1; }
    if (svgwin_open(&dev, 3) != RV9_IO_OK) { puts("open failed"); return 1; }

    /* What the driver asked for, computed the way it computes it. */
    long band = (long)PW * BAND_ROWS * 2;
    long cov  = (long)PW * 2;
    long fixed = SRC_MAX + MAX_PTS * 2 * 4;
    long bands = (PH + BAND_ROWS - 1) / BAND_ROWS;

    double t0 = now_ms();
    for (int i = 0; i < reps; i++) {
        size_t done = 0;
        blits = blit_px = 0;
        svgwin_write(&dev, buf, n, &done);
    }
    double ms = (now_ms() - t0) / reps;

    printf("%5d x %-5d  src %4zu B  buffers %6ld B (band %ld + cov %ld + "
           "fixed %ld)  bands %3ld  blits %4ld  %8.2f ms\n",
           PW, PH, n, band + cov + fixed, band, cov, fixed, bands, blits, ms);

    if (n >= SRC_MAX) {
        printf("        NOTE: the source is %zu bytes and SRC_MAX is %d -- "
               "this document does not fit\n", n, SRC_MAX);
    }
    return 0;
}
