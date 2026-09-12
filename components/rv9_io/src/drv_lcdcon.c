/*
 * lcdcon driver -- a text console on the ST7789 panel.
 *
 * Keeps a character grid and repaints only the rows that changed. Scrolling
 * shifts the grid and marks everything dirty; at 21x40 cells that is cheap
 * enough not to need hardware scroll.
 *
 * The panel is driven through ESP-IDF's esp_lcd for now. That is a shim at
 * the layer where shims belong -- drivers are where hardware knowledge is
 * allowed to live, and replacing this with RV-9's own SPI and ST7789 code
 * changes nothing above it.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

#include "panel.h"

#include "esp_log.h"

static const char *TAG = "rv9-lcdcon";

/*
 * Descriptor options, by index into rv9_devdesc_t.opt:
 *   opt[2]  glyph scale (1..4). The panel is 1.47 inches; at scale 1 a
 *           glyph is about 0.8 mm tall, which is legible only in theory.
 *   opt[3]  0 = portrait (172x320), 1 = landscape (320x172)
 *   opt[4]  foreground colour, RGB565
 *   opt[5]  background colour, RGB565
 *   opt[6]  left/right margin in pixels
 *   opt[7]  top/bottom margin in pixels
 *
 * Colours are RGB565: rrrrrggg gggbbbbb. Both zero means "use the
 * defaults" -- black on black would be useless anyway.
 *
 * Both live in the descriptor rather than here, so changing how the console
 * looks means editing a descriptor module -- not rebuilding the kernel.
 */
#define OPT_SCALE   2
#define OPT_ROTATE  3
#define OPT_FG      4
#define OPT_BG      5
#define OPT_MARGIN_X 6
#define OPT_MARGIN_Y 7

/* Asked of the panel at init; kept because the cell arithmetic needs them
   before anything has been drawn. */
#define PANEL_W       172
#define PANEL_H       320

/* Glyphs are already 10x20 and anti-aliased, so 1x is the normal case.
   Scaling above that magnifies an already-large cell. */
/* Glyph rows expanded per blit. GLYPH_H is 20; four slices of five keep
   the buffer at a quarter of what a whole row would need. */
#define GLYPH_CHUNK    5

#define DEFAULT_SCALE  1
#define MAX_SCALE      3

/* The panel's corners are rounded and its edge column sits under the bezel,
   so text drawn flush to x=0 loses part of its first character. */
#define DEFAULT_MARGIN_X 6
#define DEFAULT_MARGIN_Y 4

/* Waveshare ESP32-C5-LCD-1.47 */

#define GLYPH_W       10
#define GLYPH_H       20
#define GLYPH_STRIDE  ((GLYPH_W + 1) / 2)   /* two 4-bit pixels per byte */
#define SHADES        16

#define FONT_FIRST    32
#define FONT_LAST     126

/* 4 bits of coverage per pixel; see tools/mkfont.py. */
extern const unsigned char rv9_font[FONT_LAST - FONT_FIRST + 1]
                                   [GLYPH_H][GLYPH_STRIDE];

/* RGB565 defaults. */
#define DEFAULT_FG    0xFFFF      /* pure white */
#define DEFAULT_BG    0x0195      /* #0033aa, a medium navy */

/*
 * This panel takes RGB565 with the bytes the other way round from the
 * order esp_lcd hands them over, so colours are swapped once at init
 * rather than per pixel. Determined empirically: 0x0195 (navy) came out
 * as 0x9501 (olive) on the glass, which is the same value byte-reversed.
 *
 * White hid this for a while, 0xFFFF being symmetric.
 */
static inline uint16_t panel_color(uint16_t rgb565)
{
    return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

/*
 * The sixteen console colours, in RGB565.
 *
 * The VT100 palette, which is what RV9_COL_* names and what a program means
 * when it asks for blue. The panel's own default fg/bg come from the
 * descriptor and are *not* in here: RV9_COL_DEFAULT resolves to those, so
 * "default" keeps meaning whatever this console was configured to look
 * like rather than quietly becoming white on black.
 */
static const uint16_t s_palette[16] = {
    0x0000,  /* black          #000000 */
    0xA800,  /* red            #aa0000 */
    0x0540,  /* green          #00aa00 */
    0xAAA0,  /* yellow         #aa5500 */
    0x0015,  /* blue           #0000aa */
    0xA815,  /* magenta        #aa00aa */
    0x0555,  /* cyan           #00aaaa */
    0xAD55,  /* white          #aaaaaa */
    0x52AA,  /* bright black   #555555 */
    0xFAAA,  /* bright red     #ff5555 */
    0x57EA,  /* bright green   #55ff55 */
    0xFFEA,  /* bright yellow  #ffff55 */
    0x52BF,  /* bright blue    #5555ff */
    0xFABF,  /* bright magenta #ff55ff */
    0x57FF,  /* bright cyan    #55ffff */
    0xFFFF,  /* bright white   #ffffff */
};

/* Colour and attributes, per cell. Three bytes rather than a packed word:
   at 40x21 that is 2.5 KB, and legible code is worth more than the byte. */
typedef struct {
    uint8_t fg, bg, flags;
} cattr_t;

/*
 * Blend foreground toward background at coverage a/15, per RGB565 channel.
 * Done once per colour pair, so painting a glyph is a table lookup rather
 * than arithmetic per pixel.
 */
static uint16_t blend565(uint16_t fg, uint16_t bg, int a)
{
    int fr = (fg >> 11) & 0x1F, fgr = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = (bg >> 11) & 0x1F, bgr = (bg >> 5) & 0x3F, bb = bg & 0x1F;

    int r = br  + (fr  - br)  * a / (SHADES - 1);
    int g = bgr + (fgr - bgr) * a / (SHADES - 1);
    int b = bb  + (fb  - bb)  * a / (SHADES - 1);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

typedef struct {
    int       w, h;          /* pixels, after rotation */
    int       scale;         /* glyph magnification */
    int       cw, ch;        /* cell size in pixels */
    int       cols, rows;
    int       mx, my;        /* margins, pixels */

    char     *grid;          /* cols * rows */
    cattr_t  *attr;          /* cols * rows, colour and attributes per cell */
    bool     *dirty;         /* rows */
    uint16_t  fg, bg;        /* the descriptor's defaults, RGB565 */
    int       cx, cy;
    bool      cursor_on;
    cattr_t   pen;           /* what the next character will be written in */
    uint16_t *rowbuf;        /* one text row of pixels, w x ch */

    /* The shade table is rebuilt whenever the colour pair changes while
       painting a row. Console output comes in runs of one colour, so in
       practice this is rebuilt a handful of times per row, not per cell. */
    uint16_t  shade[SHADES];
    uint16_t  shade_fg, shade_bg;
    bool      shade_valid;

    rv9_lock_t lock;
} lcdcon_t;

static inline char *cell(lcdcon_t *c, int row, int col)
{
    return &c->grid[row * c->cols + col];
}

static inline cattr_t *cattr(lcdcon_t *c, int row, int col)
{
    return &c->attr[row * c->cols + col];
}

/* RV9_COL_DEFAULT means "whatever this console was configured to look
   like", which is why the defaults are kept as colours and not as palette
   indices -- the descriptor's navy is not one of the sixteen. */
static inline uint16_t resolve_fg(lcdcon_t *c, uint8_t v)
{
    return (v == RV9_COL_DEFAULT) ? c->fg : s_palette[v & 15];
}

static inline uint16_t resolve_bg(lcdcon_t *c, uint8_t v)
{
    return (v == RV9_COL_DEFAULT) ? c->bg : s_palette[v & 15];
}

/* The pair a cell is actually painted in, after bold and reverse have had
   their say. Bold is the bright half of the palette, which is what it has
   meant on a colour terminal since it stopped meaning a heavier font. */
static void cell_colours(lcdcon_t *c, const cattr_t *a,
                         uint16_t *fg, uint16_t *bg)
{
    uint8_t f = a->fg;
    if ((a->flags & RV9_CON_ATTR_BOLD) && f != RV9_COL_DEFAULT) {
        f |= RV9_COL_BRIGHT;
    }

    uint16_t cf = resolve_fg(c, f);
    uint16_t cb = resolve_bg(c, a->bg);

    if (a->flags & RV9_CON_ATTR_REVERSE) { uint16_t t = cf; cf = cb; cb = t; }

    *fg = cf;
    *bg = cb;
}


/* ---- painting ---- */

/* Rebuild the 16-shade ramp, but only when the colours actually changed. */
static void use_colours(lcdcon_t *c, uint16_t fg, uint16_t bg)
{
    if (c->shade_valid && c->shade_fg == fg && c->shade_bg == bg) return;

    for (int a = 0; a < SHADES; a++) c->shade[a] = panel_color(blend565(fg, bg, a));
    c->shade_fg = fg;
    c->shade_bg = bg;
    c->shade_valid = true;
}

/*
 * Expand part of a text row into pixels, magnifying each glyph by c->scale.
 *
 * Columns are the outer loop, which is the opposite of the obvious order
 * and the reason is colour: every cell may have its own pair, and the
 * 16-shade ramp that makes an anti-aliased glyph cheap has to be built per
 * pair. Walking a column at a time means building it once per run of equal
 * colour instead of once per scanline.
 *
 * Only GLYPH_CHUNK glyph rows are expanded at a time. A whole 20-pixel row
 * of a 320-wide panel is 12.8 KB of buffer held from boot to shutdown --
 * comfortably the largest single allocation in the system, on a board with
 * about forty free. Painting it in four slices costs three extra blits and
 * three extra shade-ramp rebuilds per text row, and gives nine and a half
 * kilobytes back to everything else, permanently.
 */
static void paint_slice(lcdcon_t *c, int row, int g0, int g1)
{
    uint16_t *px = c->rowbuf;
    int       gh = g1 - g0;

    /* Margins and the gaps between cells belong to nobody, so they take the
       console's own background. */
    for (int gy = 0; gy < gh; gy++) {
        uint16_t *line = &px[(gy * c->scale) * c->w];
        uint16_t bg = panel_color(c->bg);
        for (int x = 0; x < c->w; x++) line[x] = bg;
    }

    c->shade_valid = false;

    for (int col = 0; col < c->cols; col++) {
        char ch = *cell(c, row, col);
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = ' ';

        cattr_t a = *cattr(c, row, col);

        /* The cursor is a reversed cell rather than a shape of its own: it
           costs nothing to paint, survives scrolling, and is legible
           whatever colours the program chose. */
        if (c->cursor_on && row == c->cy && col == c->cx) {
            a.flags ^= RV9_CON_ATTR_REVERSE;
        }

        uint16_t fg, bg;
        cell_colours(c, &a, &fg, &bg);
        use_colours(c, fg, bg);

        for (int gy = g0; gy < g1; gy++) {
            const unsigned char *g = rv9_font[ch - FONT_FIRST][gy];
            uint16_t *out = &px[((gy - g0) * c->scale) * c->w + c->mx + col * c->cw];

            /* The last glyph row is the underline, when there is one: full
               coverage across the cell rather than the glyph's own shape. */
            bool rule = (a.flags & RV9_CON_ATTR_UNDERLINE) && gy == GLYPH_H - 1;

            for (int gx = 0; gx < GLYPH_W; gx++) {
                unsigned char byte = g[gx >> 1];
                int cov = (gx & 1) ? (byte & 0x0F) : (byte >> 4);
                uint16_t v = c->shade[rule ? SHADES - 1 : cov];
                for (int sx = 0; sx < c->scale; sx++) *out++ = v;
            }
        }
    }

    /* Replicate each expanded line downward rather than recomputing it. */
    for (int gy = 0; gy < gh && c->scale > 1; gy++) {
        uint16_t *line = &px[(gy * c->scale) * c->w];
        for (int sy = 1; sy < c->scale; sy++) {
            memcpy(&px[(gy * c->scale + sy) * c->w], line,
                   (size_t)c->w * sizeof(uint16_t));
        }
    }

    int y = c->my + row * c->ch + g0 * c->scale;
    rv9_panel_blit(0, y, c->w, y + gh * c->scale, c->rowbuf);
}

static void paint_row(lcdcon_t *c, int row)
{
    for (int g0 = 0; g0 < GLYPH_H; g0 += GLYPH_CHUNK) {
        int g1 = g0 + GLYPH_CHUNK;
        if (g1 > GLYPH_H) g1 = GLYPH_H;
        paint_slice(c, row, g0, g1);
    }
}

static void flush(lcdcon_t *c)
{
    /* If the window device has had the panel since we last painted, what
       is on the glass is its picture, not our idea of the screen. Every
       row is stale, whatever the dirty flags say. */
    if (rv9_panel_take(c)) {
        for (int r = 0; r < c->rows; r++) c->dirty[r] = true;
    }

    for (int r = 0; r < c->rows; r++) {
        if (!c->dirty[r]) continue;
        paint_row(c, r);
        c->dirty[r] = false;
    }
}

/* Fill a run of cells with blanks in the current pen. Clearing takes the
   pen's background, which is what a terminal does and what a program
   painting a coloured panel expects. */
static void blank(lcdcon_t *c, int row, int from, int to)
{
    for (int col = from; col < to; col++) {
        *cell(c, row, col)  = ' ';
        *cattr(c, row, col) = c->pen;
    }
    c->dirty[row] = true;
}

static void scroll(lcdcon_t *c)
{
    memmove(c->grid, cell(c, 1, 0), (size_t)(c->rows - 1) * c->cols);
    memmove(c->attr, cattr(c, 1, 0),
            (size_t)(c->rows - 1) * c->cols * sizeof(cattr_t));
    blank(c, c->rows - 1, 0, c->cols);
    for (int r = 0; r < c->rows; r++) c->dirty[r] = true;
    c->cy = c->rows - 1;
}

static void newline(lcdcon_t *c)
{
    c->cx = 0;
    if (++c->cy >= c->rows) scroll(c);
}

static void putch(lcdcon_t *c, char ch)
{
    switch (ch) {
    case '\n':
        newline(c);
        return;
    case '\r':
        c->cx = 0;
        return;
    case '\t':
        do { putch(c, ' '); } while (c->cx % 4 != 0);
        return;
    case '\b':
        if (c->cx > 0) {
            c->cx--;
            *cell(c, c->cy, c->cx)  = ' ';
            *cattr(c, c->cy, c->cx) = c->pen;
            c->dirty[c->cy] = true;
        }
        return;
    default:
        break;
    }

    if (ch < FONT_FIRST || ch > FONT_LAST) return;

    if (c->cx >= c->cols) newline(c);

    *cell(c, c->cy, c->cx)  = ch;
    *cattr(c, c->cy, c->cx) = c->pen;
    c->cx++;
    c->dirty[c->cy] = true;
}

/* ---- driver interface ---- */

static rv9_io_err_t lcdcon_init(rv9_dev_t *dev)
{
    lcdcon_t *c = rv9_calloc(1, sizeof(*c));
    if (c == NULL) return RV9_IO_ERR_NOMEM;

    int scale = (int)dev->opt[OPT_SCALE];
    if (scale < 1) scale = DEFAULT_SCALE;
    if (scale > MAX_SCALE) scale = MAX_SCALE;

    bool landscape = dev->opt[OPT_ROTATE] != 0;

    uint16_t fg = (uint16_t)dev->opt[OPT_FG];
    uint16_t bg = (uint16_t)dev->opt[OPT_BG];
    if (fg == 0 && bg == 0) {
        fg = DEFAULT_FG;
        bg = DEFAULT_BG;
    }
    /* Kept unswapped: these are colours, and panel_color() is applied where
       they are written to the glass. Storing them pre-swapped meant the
       blend arithmetic ran on byte-reversed channels. */
    c->fg = fg;
    c->bg = bg;

    c->pen.fg    = RV9_COL_DEFAULT;
    c->pen.bg    = RV9_COL_DEFAULT;
    c->pen.flags = 0;

    c->mx = dev->opt[OPT_MARGIN_X] ? (int)dev->opt[OPT_MARGIN_X] : DEFAULT_MARGIN_X;
    c->my = dev->opt[OPT_MARGIN_Y] ? (int)dev->opt[OPT_MARGIN_Y] : DEFAULT_MARGIN_Y;

    c->scale = scale;
    c->cw    = GLYPH_W * scale;
    c->ch    = GLYPH_H * scale;
    c->w     = landscape ? PANEL_H : PANEL_W;
    c->h     = landscape ? PANEL_W : PANEL_H;
    c->cols  = (c->w - 2 * c->mx) / c->cw;
    c->rows  = (c->h - 2 * c->my) / c->ch;

    if (c->cols < 1 || c->rows < 1) {
        rv9_free(c);
        return RV9_IO_ERR_INVAL;
    }

    c->grid  = rv9_alloc((size_t)c->cols * c->rows);
    c->attr  = rv9_alloc((size_t)c->cols * c->rows * sizeof(cattr_t));
    c->dirty = rv9_calloc((size_t)c->rows, sizeof(bool));
    c->rowbuf = rv9_alloc_dma((size_t)c->w * GLYPH_CHUNK * c->scale *
                              sizeof(uint16_t));

    if (c->grid == NULL || c->attr == NULL || c->dirty == NULL ||
        c->rowbuf == NULL || rv9_lock_create(&c->lock) != RV9_OK) {
        rv9_free(c->grid);
        rv9_free(c->attr);
        rv9_free(c->dirty);
        rv9_free(c->rowbuf);
        rv9_free(c);
        return RV9_IO_ERR_NOMEM;
    }

    memset(c->grid, ' ', (size_t)c->cols * c->rows);
    for (int i = 0; i < c->cols * c->rows; i++) c->attr[i] = c->pen;

    /* The glass belongs to panel.c, because `/w0` wants it too. This
       brings it up if nobody has yet, and otherwise just reports it. */
    rv9_io_err_t perr = rv9_panel_open(landscape, NULL, NULL);
    if (perr != RV9_IO_OK) {
        rv9_free(c->grid);
        rv9_free(c->attr);
        rv9_free(c->dirty);
        rv9_free(c->rowbuf);
        rv9_free(c);
        return perr;
    }
    rv9_panel_backlight(100);

    /* Clear the whole panel, margins included, so we neither inherit what
       was on it nor leave unpainted bands around the text area.
       A strip at a time, because that is how big the buffer is -- the
       clear and the buffer have to agree, and when they stopped agreeing
       this wrote four times past the end of it. */
    int strip = GLYPH_CHUNK * c->scale;

    for (int x = 0; x < c->w * strip; x++) c->rowbuf[x] = panel_color(c->bg);
    for (int y = 0; y < c->h; y += strip) {
        int y2 = y + strip;
        if (y2 > c->h) y2 = c->h;
        rv9_panel_blit(0, y, c->w, y2, c->rowbuf);
    }

    for (int r = 0; r < c->rows; r++) c->dirty[r] = true;
    flush(c);

    dev->drv_state = c;
    ESP_LOGI(TAG, "console up: %dx%d cells, %dx%d px, %dx glyphs, %s, "
                  "fg %04x on bg %04x (margins %d/%d)",
             c->cols, c->rows, c->w, c->h, c->scale,
             landscape ? "landscape" : "portrait", fg, bg, c->mx, c->my);
    return RV9_IO_OK;
}

static rv9_io_err_t lcdcon_write(rv9_dev_t *dev, const void *buf, size_t len,
                                 size_t *done)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;

    const char *s = (const char *)buf;

    rv9_lock_acquire(c->lock);

    /* The row the cursor is leaving has to be repainted without it. */
    int was = c->cy;
    for (size_t i = 0; i < len; i++) putch(c, s[i]);
    if (c->cursor_on && c->cy != was) c->dirty[was] = true;
    if (c->cursor_on) c->dirty[c->cy] = true;

    flush(c);
    rv9_lock_release(c->lock);

    if (done) *done = len;
    return RV9_IO_OK;
}

/*
 * Backlight brightness.
 *
 * The backlight was switched on at boot and left there, which is the
 * largest continuous draw on this board and a real contributor to how warm
 * it runs. It is a PWM output like any other, so it may as well be
 * adjustable -- and being able to turn the panel down without turning the
 * system off is worth having on anything battery-powered or enclosed.
 *
 * Runtime only: not remembered across a reset, because a machine that
 * boots with a dark display is unnecessarily hard to diagnose.
 */
static void backlight_set(lcdcon_t *c, uint32_t percent)
{
    (void)c;
    rv9_panel_backlight(percent);
}

static void do_clear(lcdcon_t *c, uint32_t what)
{
    switch (what) {
    case RV9_CON_CLEAR_EOL:
        blank(c, c->cy, c->cx, c->cols);
        break;

    case RV9_CON_CLEAR_EOS:
        blank(c, c->cy, c->cx, c->cols);
        for (int r = c->cy + 1; r < c->rows; r++) blank(c, r, 0, c->cols);
        break;

    default:
        for (int r = 0; r < c->rows; r++) blank(c, r, 0, c->cols);
        c->cx = c->cy = 0;
        break;
    }
}

/*
 * The console settings, answered natively.
 *
 * This is the half of the arrangement that makes it worth having: /term has
 * a character grid, so it moves a render position and paints a cell in a
 * colour. SCF never gets as far as its escape sequences here, and the
 * program that called setstat cannot tell the difference -- which is the
 * entire claim.
 */
static rv9_io_err_t lcdcon_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;
    if (arg == NULL) return RV9_IO_ERR_INVAL;

    uint32_t v = *(uint32_t *)arg;

    if (code == RV9_LCD_SS_BRIGHTNESS) {
        backlight_set(c, v);
        return RV9_IO_OK;
    }

    rv9_lock_acquire(c->lock);
    int was = c->cy;
    rv9_io_err_t err = RV9_IO_OK;

    switch (code) {
    case RV9_CON_SS_CURSOR: {
        int row = (int)((v >> 16) & 0xFFFF);
        int col = (int)(v & 0xFFFF);
        if (row >= c->rows) row = c->rows - 1;
        if (col >= c->cols) col = c->cols - 1;
        c->cy = row;
        c->cx = col;
        break;
    }

    case RV9_CON_SS_COLOUR:
        c->pen.fg = (uint8_t)(v & 0xFF);
        c->pen.bg = (uint8_t)((v >> 8) & 0xFF);
        break;

    case RV9_CON_SS_ATTR:
        c->pen.flags = (uint8_t)(v & 7);
        break;

    case RV9_CON_SS_CURSOR_ON:
        c->cursor_on = (v != 0);
        break;

    case RV9_CON_SS_CLEAR:
        do_clear(c, v);
        break;

    case RV9_LCD_SS_CLEAR:
        do_clear(c, RV9_CON_CLEAR_SCREEN);
        break;

    default:
        err = RV9_IO_ERR_UNSUPPORTED;
        break;
    }

    if (err == RV9_IO_OK && c->cursor_on) {
        c->dirty[was]   = true;
        c->dirty[c->cy] = true;
    }
    if (err == RV9_IO_OK) flush(c);

    rv9_lock_release(c->lock);
    return err;
}

static rv9_io_err_t lcdcon_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    if (c == NULL || arg == NULL) return RV9_IO_ERR_IO;

    switch (code) {
    case RV9_LCD_SS_BRIGHTNESS:
        *(uint32_t *)arg = rv9_panel_backlight_get();
        return RV9_IO_OK;

    case RV9_CON_GS_SIZE:
        /* This console knows exactly how big it is, which is the one thing
           a serial line can never say. */
        *(uint32_t *)arg = ((uint32_t)c->rows << 16) | (uint32_t)c->cols;
        return RV9_IO_OK;

    case RV9_CON_SS_CURSOR:
        *(uint32_t *)arg = ((uint32_t)c->cy << 16) | (uint32_t)c->cx;
        return RV9_IO_OK;

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_driver_t lcdcon = {
    .name    = "lcdcon",
    .init    = lcdcon_init,
    .write   = lcdcon_write,
    .getstat = lcdcon_getstat,
    .setstat = lcdcon_setstat,
};

rv9_io_err_t rv9_drv_lcdcon_register(void)
{
    return rv9_io_register_driver(&lcdcon);
}
