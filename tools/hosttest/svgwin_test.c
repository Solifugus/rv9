/*
 * Three things the window device does that nothing else checks.
 *
 * Written 2026-09-30 by the whisker session (~/development/whisker) while
 * reviewing this driver as a substrate, and landed here with the two defects
 * it found fixed. Each check is one paragraph of docs/roadmap.md made
 * executable:
 *
 *   1  a document over SRC_MAX must not wedge the path
 *   2  a raw '<' in text content must return
 *   3  a clipped repaint must carry backing for its whole *bands*
 *
 * 1 and 2 both failed when this was written. The window swallowed every
 * document after an oversized one, for the life of the open path and without
 * a word in the log; and `<text>a < b</text>` did not draw wrongly, it never
 * returned -- an infinite loop inside svgwin_write with the lock held, on one
 * core. Both are fixed in drv_svgwin.c and this is what keeps them fixed.
 *
 * 3 is not a defect. It is the rule on RV9_SVG_SS_ROWS written down as a
 * test, and writing it down is how the rule turned out to be half-right:
 * "carry the backing for the widget's rows" is wrong, because the clip snaps
 * outward and 100..140 repaints 96..143. `flick` was correct only because it
 * picks band-aligned rows on purpose.
 *
 * Check 3 carries its own control -- the scene must first paint the row in
 * something other than the device colour -- so the file cannot pass by having
 * rendered nothing.
 */
#include "drv_svgwin.c"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PW 320
#define PH 172

static uint16_t screen[PW * PH];
static int blits;
static int fails;

rv9_io_err_t rv9_panel_open(bool l, int *w, int *h)
{ (void)l; if (w) *w = PW; if (h) *h = PH; return RV9_IO_OK; }
void rv9_panel_size(int *w, int *h) { if (w) *w = PW; if (h) *h = PH; }
void rv9_panel_backlight(uint32_t p) { (void)p; }
uint32_t rv9_panel_backlight_get(void) { return 100; }
bool rv9_panel_take(const void *o) { (void)o; return false; }

void rv9_panel_blit(int x0, int y0, int x1, int y1, const uint16_t *px)
{
    blits++;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            screen[y * PW + x] = px[(y - y0) * (x1 - x0) + (x - x0)];
}

static uint16_t at(int x, int y)
{
    return (uint16_t)((screen[y * PW + x] >> 8) | (screen[y * PW + x] << 8));
}

static void put(rv9_dev_t *dev, const char *doc)
{
    size_t done;
    svgwin_write(dev, doc, strlen(doc), &done);
}

static void ok(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

/* ------------------------------------------------------------------ */

/*
 * 1. A document over SRC_MAX does not truncate the picture -- it stops the
 *    window answering, for the life of the open path.
 *
 *    Past SRC_MAX the bytes are dropped (svgwin_write), so the "</svg>" that
 *    ends a document can never be the last six bytes of the buffer again,
 *    render() is never called, and s->len is never reset. Nothing is logged
 *    either: the "document over %d bytes" warning is inside the branch that
 *    no longer runs.
 */
static void t_overflow(rv9_dev_t *dev)
{
    static char big[6000];
    const char *good = "<svg viewBox=\"0 0 320 172\">"
                       "<rect width=\"320\" height=\"172\" fill=\"#123456\"/></svg>";
    int n = snprintf(big, sizeof big, "<svg viewBox=\"0 0 320 172\">");

    while (n < 5000)
        n += snprintf(big + n, sizeof big - n,
                      "<rect x=\"1\" y=\"1\" width=\"9\" height=\"9\" fill=\"#abc\"/>");
    n += snprintf(big + n, sizeof big - n, "</svg>");

    blits = 0;
    put(dev, big);
    printf("     the oversized document drew %d bands\n", blits);

    blits = 0;
    put(dev, good);
    ok(blits > 0, "a document after an oversized one still draws");
}

/*
 * 2. A raw '<' in text content never returns.
 *
 *    attr() scans an attribute name, stops on '/' or '>', and then does
 *    `if (p >= tend || *p != '=') continue;` without advancing p. A raw '<'
 *    in content makes the scanner read the rest of the text as a tag, and
 *    every text element ends "</text>", so the scan always reaches a '/'.
 *    The loop is inside svgwin_write, holding the lock, on one core.
 *
 *    A label is data. This is reachable from a program printing "a < b".
 */
static void t_raw_lt(rv9_dev_t *dev)
{
    const char *doc = "<svg viewBox=\"0 0 320 172\">"
                      "<text x=\"10\" y=\"40\" font-size=\"20\" fill=\"#fff\">a < b</text>"
                      "</svg>";
    pid_t kid;

    fflush(stdout);
    kid = fork();
    if (kid == 0) { put(dev, doc); _exit(0); }

    for (int ms = 0; ms < 3000; ms += 20) {
        int st;
        if (waitpid(kid, &st, WNOHANG) == kid) {
            ok(1, "a raw '<' in text content returns");
            return;
        }
        usleep(20000);
    }
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);
    ok(0, "a raw '<' in text content returns (3 s, where a frame is 0.3 ms)");
}

/*
 * 3. A clipped repaint must carry backing for its whole bands.
 *
 *    The clip snaps outward and every band is cleared to the device
 *    background before anything is drawn into it, so a backing rect covering
 *    only the widget's rows leaves a strip of device colour at each end.
 *    This is the rule on RV9_SVG_SS_ROWS; `flick` learned it the hard way.
 */
static void t_band_backing(rv9_dev_t *dev)
{
    uint32_t rows = (100u << 16) | 140u;       /* deliberately not band-aligned */
    uint16_t navy;                             /* read from the scene, not assumed */

    const char *scene =
        "<svg viewBox=\"0 0 320 172\">"
        "<rect width=\"320\" height=\"172\" fill=\"#10203a\"/>"
        "<rect x=\"40\" y=\"100\" width=\"240\" height=\"40\" fill=\"#2a6b8f\"/></svg>";
    const char *tight =                        /* backing over 100..140 only */
        "<svg viewBox=\"0 0 320 172\">"
        "<rect x=\"0\" y=\"100\" width=\"320\" height=\"40\" fill=\"#10203a\"/>"
        "<rect x=\"40\" y=\"100\" width=\"240\" height=\"40\" fill=\"#8fd0ff\"/></svg>";
    const char *snapped =                      /* backing over 96..144 */
        "<svg viewBox=\"0 0 320 172\">"
        "<rect x=\"0\" y=\"96\" width=\"320\" height=\"48\" fill=\"#10203a\"/>"
        "<rect x=\"40\" y=\"100\" width=\"240\" height=\"40\" fill=\"#8fd0ff\"/></svg>";

    /* The control: without it, "the strip is navy" is satisfied by a
       harness that never repainted anything. */
    put(dev, scene);
    navy = at(10, 97);
    ok(navy != 0x0000, "the scene paints row 97 in something other than the device colour");

    svgwin_setstat(dev, RV9_SVG_SS_ROWS, &rows);
    put(dev, tight);
    ok(at(10, 97) != navy,
       "backing over the widget's rows alone loses row 97 (the known trap)");

    put(dev, scene);
    svgwin_setstat(dev, RV9_SVG_SS_ROWS, &rows);
    put(dev, snapped);
    ok(at(10, 97) == navy, "backing snapped out to the bands keeps row 97");
    ok(at(10, 142) == navy, "and keeps row 142, at the other end");
}

int main(void)
{
    rv9_dev_t dev;

    memset(&dev, 0, sizeof dev);
    dev.opt[OPT_ROTATE] = 1;
    dev.opt[OPT_BG] = 0x0000;
    svgwin_init(&dev);
    svgwin_open(&dev, 3);

    t_overflow(&dev);
    svgwin_close(&dev);                        /* t_overflow leaves it wedged */
    svgwin_open(&dev, 3);

    t_raw_lt(&dev);
    t_band_backing(&dev);

    printf(fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
