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
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "rv9-lcdcon";

/* Waveshare ESP32-C5-LCD-1.47 */
#define LCD_HOST      SPI2_HOST
#define PIN_SCLK      7
#define PIN_MOSI      6
#define PIN_CS        23
#define PIN_DC        24
#define PIN_RST       26
#define PIN_BL        10

#define LCD_W         172
#define LCD_H         320
#define LCD_X_GAP     34    /* 172-wide panel centred in the controller's 240 */
#define LCD_Y_GAP     0
#define LCD_CLK_HZ    (40 * 1000 * 1000)

#define GLYPH_W       8
#define GLYPH_H       8
#define COLS          (LCD_W / GLYPH_W)    /* 21 */
#define ROWS          (LCD_H / GLYPH_H)    /* 40 */

#define FONT_FIRST    32
#define FONT_LAST     126

extern const unsigned char rv9_font8x8[FONT_LAST - FONT_FIRST + 1][8];

/* RGB565. data_endian is set to big below, so these are written as-is. */
#define COLOR_FG      0xC618      /* light grey */
#define COLOR_BG      0x0000      /* black */

typedef struct {
    esp_lcd_panel_handle_t panel;
    char      grid[ROWS][COLS];
    bool      dirty[ROWS];
    int       cx, cy;
    uint16_t *rowbuf;        /* one text row of pixels, LCD_W x GLYPH_H */
    rv9_mutex_t lock;
} lcdcon_t;

/* ---- painting ---- */

static void paint_row(lcdcon_t *c, int row)
{
    uint16_t *px = c->rowbuf;

    for (int gy = 0; gy < GLYPH_H; gy++) {
        for (int col = 0; col < COLS; col++) {
            char ch = c->grid[row][col];
            unsigned char bits = 0;
            if (ch >= FONT_FIRST && ch <= FONT_LAST) {
                bits = rv9_font8x8[ch - FONT_FIRST][gy];
            }
            for (int gx = 0; gx < GLYPH_W; gx++) {
                px[gy * LCD_W + col * GLYPH_W + gx] =
                    (bits & (0x80 >> gx)) ? COLOR_FG : COLOR_BG;
            }
        }
    }

    esp_lcd_panel_draw_bitmap(c->panel, 0, row * GLYPH_H,
                              LCD_W, row * GLYPH_H + GLYPH_H, c->rowbuf);
}

static void flush(lcdcon_t *c)
{
    for (int r = 0; r < ROWS; r++) {
        if (!c->dirty[r]) continue;
        paint_row(c, r);
        c->dirty[r] = false;
    }
}

static void scroll(lcdcon_t *c)
{
    memmove(&c->grid[0], &c->grid[1], (ROWS - 1) * COLS);
    memset(&c->grid[ROWS - 1], ' ', COLS);
    for (int r = 0; r < ROWS; r++) c->dirty[r] = true;
    c->cy = ROWS - 1;
}

static void newline(lcdcon_t *c)
{
    c->cx = 0;
    if (++c->cy >= ROWS) scroll(c);
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
            c->grid[c->cy][c->cx] = ' ';
            c->dirty[c->cy] = true;
        }
        return;
    default:
        break;
    }

    if (ch < FONT_FIRST || ch > FONT_LAST) return;

    if (c->cx >= COLS) newline(c);

    c->grid[c->cy][c->cx++] = ch;
    c->dirty[c->cy] = true;
}

/* ---- driver interface ---- */

static rv9_io_err_t lcdcon_init(rv9_dev_t *dev)
{
    lcdcon_t *c = rv9_calloc(1, sizeof(*c));
    if (c == NULL) return RV9_IO_ERR_NOMEM;

    c->rowbuf = rv9_alloc_dma(LCD_W * GLYPH_H * sizeof(uint16_t));
    if (c->rowbuf == NULL) {
        rv9_free(c);
        return RV9_IO_ERR_NOMEM;
    }

    if (rv9_mutex_create(&c->lock) != RV9_OK) {
        rv9_free(c->rowbuf);
        rv9_free(c);
        return RV9_IO_ERR_NOMEM;
    }

    memset(c->grid, ' ', sizeof(c->grid));

    /* Backlight on. PWM dimming would be a setstat; not needed yet. */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * GLYPH_H * sizeof(uint16_t) + 64,
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
    esp_lcd_panel_set_gap(c->panel, LCD_X_GAP, LCD_Y_GAP);
    esp_lcd_panel_invert_color(c->panel, true);   /* ST7789 panels are IPS */
    esp_lcd_panel_disp_on_off(c->panel, true);

    /* Clear the screen so we do not inherit whatever was on it. */
    for (int r = 0; r < ROWS; r++) c->dirty[r] = true;
    flush(c);

    dev->drv_state = c;
    ESP_LOGI(TAG, "console up: %dx%d cells (%dx%d px)", COLS, ROWS, LCD_W, LCD_H);
    return RV9_IO_OK;
}

static rv9_io_err_t lcdcon_write(rv9_dev_t *dev, const void *buf, size_t len,
                                 size_t *done)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;

    const char *s = (const char *)buf;

    rv9_mutex_lock(c->lock, RV9_WAIT_FOREVER);
    for (size_t i = 0; i < len; i++) putch(c, s[i]);
    flush(c);
    rv9_mutex_unlock(c->lock);

    if (done) *done = len;
    return RV9_IO_OK;
}

/* Driver-private setstat codes. */
#define LCDCON_SS_CLEAR (RV9_SS_DRIVER_BASE + 0)

static rv9_io_err_t lcdcon_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    lcdcon_t *c = (lcdcon_t *)dev->drv_state;
    (void)arg;

    if (code != LCDCON_SS_CLEAR) return RV9_IO_ERR_UNSUPPORTED;

    rv9_mutex_lock(c->lock, RV9_WAIT_FOREVER);
    memset(c->grid, ' ', sizeof(c->grid));
    c->cx = c->cy = 0;
    for (int r = 0; r < ROWS; r++) c->dirty[r] = true;
    flush(c);
    rv9_mutex_unlock(c->lock);

    return RV9_IO_OK;
}

static const rv9_driver_t lcdcon = {
    .name    = "lcdcon",
    .init    = lcdcon_init,
    .write   = lcdcon_write,
    .setstat = lcdcon_setstat,
};

rv9_io_err_t rv9_drv_lcdcon_register(void)
{
    return rv9_io_register_driver(&lcdcon);
}
