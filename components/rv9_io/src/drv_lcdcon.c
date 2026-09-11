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

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
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

/* Glyphs are already 10x20 and anti-aliased, so 1x is the normal case.
   Scaling above that magnifies an already-large cell. */
#define DEFAULT_SCALE  1
#define MAX_SCALE      3

/* The panel's corners are rounded and its edge column sits under the bezel,
   so text drawn flush to x=0 loses part of its first character. */
#define DEFAULT_MARGIN_X 6
#define DEFAULT_MARGIN_Y 4

/* Waveshare ESP32-C5-LCD-1.47 */
#define LCD_HOST      SPI2_HOST
#define PIN_SCLK      7
#define PIN_MOSI      6
#define PIN_CS        23
#define PIN_DC        24
#define PIN_RST       26
#define PIN_BL        10

#define PANEL_W       172   /* native, before rotation */
#define PANEL_H       320
#define PANEL_GAP     34    /* 172-wide panel centred in the controller's 240 */
#define LCD_CLK_HZ    (40 * 1000 * 1000)

/* Backlight PWM. Above the channels /pwm0 allocates, on its own timer. */
#define BL_CHANNEL    LEDC_CHANNEL_5
#define BL_TIMER      LEDC_TIMER_1
#define BL_DUTY_BITS  LEDC_TIMER_10_BIT
#define BL_FREQ_HZ    5000

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
 * Blend foreground toward background at coverage a/15, per RGB565 channel.
 * Done once into a 16-entry palette at init, so painting a glyph is a table
 * lookup rather than arithmetic per pixel.
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
    esp_lcd_panel_handle_t panel;

    int       w, h;          /* pixels, after rotation */
    int       scale;         /* glyph magnification */
    int       cw, ch;        /* cell size in pixels */
    int       cols, rows;
    int       mx, my;        /* margins, pixels */

    char     *grid;          /* cols * rows */
    bool     *dirty;         /* rows */
    uint16_t  fg, bg;
    uint8_t   brightness;      /* percent */
    uint16_t  shade[SHADES];   /* fg blended to bg, ready for the panel */
    int       cx, cy;
    uint16_t *rowbuf;        /* one text row of pixels, w x ch */

    rv9_lock_t lock;
} lcdcon_t;

static inline char *cell(lcdcon_t *c, int row, int col)
{
    return &c->grid[row * c->cols + col];
}

static void backlight_set(lcdcon_t *c, uint32_t percent);

/* ---- painting ---- */

/* Expand one text row into pixels, magnifying each glyph by c->scale. */
static void paint_row(lcdcon_t *c, int row)
{
    uint16_t *px = c->rowbuf;

    for (int gy = 0; gy < GLYPH_H; gy++) {
        uint16_t *line = &px[(gy * c->scale) * c->w];

        /* Margins are part of the row, painted in background. */
        for (int x = 0; x < c->w; x++) line[x] = c->bg;

        for (int col = 0; col < c->cols; col++) {
            char ch = *cell(c, row, col);
            if (ch < FONT_FIRST || ch > FONT_LAST) ch = ' ';

            const unsigned char *g = rv9_font[ch - FONT_FIRST][gy];
            uint16_t *out = &line[c->mx + col * c->cw];

            for (int gx = 0; gx < GLYPH_W; gx++) {
                unsigned char byte = g[gx >> 1];
                int a = (gx & 1) ? (byte & 0x0F) : (byte >> 4);
                uint16_t v = c->shade[a];
                for (int sx = 0; sx < c->scale; sx++) *out++ = v;
            }
        }

        /* Replicate the expanded line downward rather than recomputing it. */
        for (int sy = 1; sy < c->scale; sy++) {
            memcpy(&px[(gy * c->scale + sy) * c->w], line,
                   (size_t)c->w * sizeof(uint16_t));
        }
    }

    int y = c->my + row * c->ch;
    esp_lcd_panel_draw_bitmap(c->panel, 0, y, c->w, y + c->ch, c->rowbuf);
}

static void flush(lcdcon_t *c)
{
    for (int r = 0; r < c->rows; r++) {
        if (!c->dirty[r]) continue;
        paint_row(c, r);
        c->dirty[r] = false;
    }
}

static void scroll(lcdcon_t *c)
{
    memmove(c->grid, cell(c, 1, 0), (size_t)(c->rows - 1) * c->cols);
    memset(cell(c, c->rows - 1, 0), ' ', (size_t)c->cols);
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
            *cell(c, c->cy, c->cx) = ' ';
            c->dirty[c->cy] = true;
        }
        return;
    default:
        break;
    }

    if (ch < FONT_FIRST || ch > FONT_LAST) return;

    if (c->cx >= c->cols) newline(c);

    *cell(c, c->cy, c->cx++) = ch;
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
    c->fg = panel_color(fg);
    c->bg = panel_color(bg);
    for (int a = 0; a < SHADES; a++) {
        c->shade[a] = panel_color(blend565(fg, bg, a));
    }

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
    c->dirty = rv9_calloc((size_t)c->rows, sizeof(bool));
    c->rowbuf = rv9_alloc_dma((size_t)c->w * c->ch * sizeof(uint16_t));

    if (c->grid == NULL || c->dirty == NULL || c->rowbuf == NULL ||
        rv9_lock_create(&c->lock) != RV9_OK) {
        rv9_free(c->grid);
        rv9_free(c->dirty);
        rv9_free(c->rowbuf);
        rv9_free(c);
        return RV9_IO_ERR_NOMEM;
    }

    memset(c->grid, ' ', (size_t)c->cols * c->rows);

    /* Backlight as a PWM output, so it can be turned down. Its own timer
       and a channel above the ones /pwm0 hands out, so the two cannot
       fight over hardware. */
    ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_DUTY_BITS,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);

    ledc_channel_config_t bl_ch = {
        .gpio_num   = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&bl_ch);

    backlight_set(c, 100);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)((size_t)c->w * c->ch * sizeof(uint16_t)) + 64,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return RV9_IO_ERR_IO;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io failed");
        return RV9_IO_ERR_IO;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &c->panel) != ESP_OK) {
        ESP_LOGE(TAG, "st7789 panel failed");
        return RV9_IO_ERR_IO;
    }

    esp_lcd_panel_reset(c->panel);
    esp_lcd_panel_init(c->panel);

    if (landscape) {
        /* Swapping axes swaps which edge the panel's offset applies to. */
        esp_lcd_panel_swap_xy(c->panel, true);
        esp_lcd_panel_mirror(c->panel, false, true);
        esp_lcd_panel_set_gap(c->panel, 0, PANEL_GAP);
    } else {
        esp_lcd_panel_set_gap(c->panel, PANEL_GAP, 0);
    }

    esp_lcd_panel_invert_color(c->panel, true);   /* ST7789 panels are IPS */
    esp_lcd_panel_disp_on_off(c->panel, true);

    /* Clear the whole panel, margins included, so we neither inherit what
       was on it nor leave unpainted bands around the text area. */
    for (int x = 0; x < c->w * c->ch; x++) c->rowbuf[x] = c->bg;
    for (int y = 0; y < c->h; y += c->ch) {
        int y2 = y + c->ch;
        if (y2 > c->h) y2 = c->h;
        esp_lcd_panel_draw_bitmap(c->panel, 0, y, c->w, y2, c->rowbuf);
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
    for (size_t i = 0; i < len; i++) putch(c, s[i]);
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
    if (percent > 100) percent = 100;
    c->brightness = (uint8_t)percent;

    /* Perceived brightness is far from linear, but a straight mapping is
       honest about what it does and predictable to script against. */
    uint32_t duty = (percent * ((1u << BL_DUTY_BITS) - 1)) / 100u;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static rv9_io_err_t lcdcon_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;

    if (code == RV9_LCD_SS_BRIGHTNESS) {
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        backlight_set(c, *(uint32_t *)arg);
        return RV9_IO_OK;
    }

    if (code != RV9_LCD_SS_CLEAR) return RV9_IO_ERR_UNSUPPORTED;

    rv9_lock_acquire(c->lock);
    memset(c->grid, ' ', (size_t)c->cols * c->rows);
    c->cx = c->cy = 0;
    for (int r = 0; r < c->rows; r++) c->dirty[r] = true;
    flush(c);
    rv9_lock_release(c->lock);

    return RV9_IO_OK;
}

static rv9_io_err_t lcdcon_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    if (c == NULL || arg == NULL) return RV9_IO_ERR_IO;

    if (code == RV9_LCD_SS_BRIGHTNESS) {
        *(uint32_t *)arg = c->brightness;
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
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
