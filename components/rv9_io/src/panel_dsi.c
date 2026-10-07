/*
 * The panel -- one EK79007 over MIPI-DSI, shared.
 *
 * Same job and the same interface as panel.c, which drives the C5's ST7789
 * over SPI: there is one piece of glass and more than one device that wants
 * to draw on it, so the panel lives below both of them. See panel.h and the
 * header of panel.c for why that split exists; this file only differs in
 * the hardware underneath it, which differs almost completely.
 *
 * WHERE THESE NUMBERS COME FROM
 *
 * Not from a datasheet nobody here has. The board's own factory firmware
 * was preserved before RV-9 was ever flashed to it (~/esp32-p4-factory, not
 * in this repo) and it names what it was built from: an EK79007 panel, a
 * GT911 touch controller, and a board support package called
 * esp32_p4_wifi6_touch_lcd_7b. That BSP is published, and its header is the
 * pin-out -- read as a datasheet, not taken as a dependency.
 *
 * The DSI timings are Espressif's esp_lcd_ek79007 defaults (Apache-2.0, the
 * same licence as this) and the eight vendor register writes below are that
 * driver's `vendor_specific_init_default`. Eight register writes is not
 * worth a component dependency, a lock file and a build that needs the
 * network, so they are here instead. The work that is actually large --
 * the DSI bus, the D-PHY, the DPI framebuffer and its scan-out -- is
 * ESP-IDF's and is bundled.
 *
 * WHAT IS DIFFERENT FROM SPI, which is most of it
 *
 * The ST7789 holds its own framebuffer and is written to a window at a
 * time. This panel holds nothing: the P4 scans the picture out of memory
 * continuously, so there is a real 1.2 MB framebuffer and it has to live in
 * PSRAM, which is why PSRAM had to be turned on before any of this could
 * work. A blit is a copy into that framebuffer rather than a transfer to a
 * controller.
 *
 * There is also no bus to share. The C5's display and card sit on one SPI
 * bus and panel.c owns bringing it up; here the display is on the DSI link
 * and the card is on SDMMC, so rv9_panel_bus_claim() has nothing to do.
 */
#include "panel.h"

#include "rv9/kal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "rv9-panel";

/* From the board support package's header, which is the only authority
   anybody here has for them. */
#define PIN_RST       33
#define PIN_BL        32

/*
 * The D-PHY's supply is one of the chip's own regulators and the display
 * is simply dark without it -- no error, no log line, nothing. LDO_VO3 is
 * the channel wired to VDD_MIPI_DPHY on this board. Channel 4 feeds the
 * card slot and is not this.
 */
#define DPHY_LDO_CHAN     3
#define DPHY_LDO_MV    2500

#define DSI_LANES         2
#define DSI_LANE_MBPS  1000      /* the board's figure, not the driver's 900 */

#define PANEL_W        1024
#define PANEL_H         600

/*
 * The glass, in micrometres.
 *
 * Derived, like the C5's: a 7 in diagonal is 177.8 mm, and 1024:600 over
 * that diagonal gives 153.4 x 89.9 mm. A tape measure would settle it
 * better than arithmetic does, and these are the numbers to correct if one
 * disagrees. Nothing asks the hardware because nothing can -- there is no
 * EDID on a DSI link of this kind.
 */
#define PANEL_W_UM   153405
#define PANEL_H_UM    89886

#define BL_TIMER      LEDC_TIMER_1
#define BL_CHANNEL    LEDC_CHANNEL_1
#define BL_DUTY_BITS  10

/* 0xB2, then the panel maker's eight. 0x11 is sleep-out and the 120 ms
   after it is the panel's, not ours. */
#define EK_PAD_CONTROL  0xB2
#define EK_TWO_LANE     0x10

typedef struct {
    uint8_t cmd;
    uint8_t data;
    bool    has_data;
    int     delay_ms;
} init_cmd_t;

static const init_cmd_t s_init[] = {
    { EK_PAD_CONTROL, EK_TWO_LANE, true,   0 },
    { 0x80,           0x8B,        true,   0 },
    { 0x81,           0x78,        true,   0 },
    { 0x82,           0x84,        true,   0 },
    { 0x83,           0x88,        true,   0 },
    { 0x84,           0xA8,        true,   0 },
    { 0x85,           0xE3,        true,   0 },
    { 0x86,           0x88,        true,   0 },
    { 0x11,           0x00,        false, 120 },   /* sleep out */
};

static esp_lcd_panel_handle_t s_panel;
static rv9_lock_t             s_lock;
static int                    s_w, s_h;
static uint8_t                s_brightness = 100;
static bool                   s_up;
static const void            *s_owner;

void rv9_panel_backlight(uint32_t percent)
{
    if (percent > 100) percent = 100;
    s_brightness = (uint8_t)percent;

    uint32_t max = (1u << BL_DUTY_BITS) - 1;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, percent * max / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

uint32_t rv9_panel_backlight_get(void)
{
    return s_brightness;
}

/* Nothing shares a bus with this panel. Kept because panel.h is the
   interface both boards answer to, and the card driver calls it. */
rv9_io_err_t rv9_panel_bus_claim(void)
{
    return RV9_IO_OK;
}

static rv9_io_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_DUTY_BITS,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) return RV9_IO_ERR_IO;

    /* Starts dark on purpose: the framebuffer holds whatever PSRAM held at
       reset, and showing that for the moment before the first clear is a
       flash of confetti on a 7 inch screen. */
    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PIN_BL,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) return RV9_IO_ERR_IO;

    return RV9_IO_OK;
}

/*
 * Bring the panel up, or report what it already is.
 *
 * `landscape` is accepted and ignored. The C5's panel is wired portrait and
 * rotating it is free, because every pixel passes through software on its
 * way to the controller. Here the hardware scans the framebuffer in one
 * fixed direction and rotation would mean turning 1.2 MB around on every
 * frame. So this panel is 1024x600 and says so, and a caller that wanted
 * otherwise gets the truth rather than a refusal -- which is the same
 * answer panel.c gives a second caller that disagrees with the first.
 */
rv9_io_err_t rv9_panel_open(bool landscape, int *w, int *h)
{
    if (s_up) {
        if (w) *w = s_w;
        if (h) *h = s_h;
        return RV9_IO_OK;
    }

    if (!landscape) {
        ESP_LOGW(TAG, "this panel is landscape only; reporting %dx%d",
                 PANEL_W, PANEL_H);
    }

    if (rv9_lock_create(&s_lock) != RV9_OK) return RV9_IO_ERR_NOMEM;

    rv9_io_err_t err = backlight_init();
    if (err != RV9_IO_OK) return err;

    /* Power to the D-PHY before anything touches the link. */
    static esp_ldo_channel_handle_t phy_pwr;
    esp_ldo_channel_config_t ldo = {
        .chan_id    = DPHY_LDO_CHAN,
        .voltage_mv = DPHY_LDO_MV,
    };
    if (esp_ldo_acquire_channel(&ldo, &phy_pwr) != ESP_OK) {
        ESP_LOGE(TAG, "no LDO channel %d for the DSI PHY", DPHY_LDO_CHAN);
        return RV9_IO_ERR_IO;
    }

    esp_lcd_dsi_bus_handle_t bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_LANES,
        /* Left at zero on purpose. The driver picks the default source
           itself and has a chip-revision special case for it; naming one
           here with the constant the vendor driver uses got an abort()
           out of the low-level switch, because IDF v6 renamed the enum
           and the old name no longer maps to anything it handles. */
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    if (esp_lcd_new_dsi_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_dsi_bus failed");
        return RV9_IO_ERR_IO;
    }

    /* Commands and parameters go over DBI; pixels go over DPI. Two
       channels on one link, which is what DSI is for. */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    if (esp_lcd_new_panel_io_dbi(bus, &dbi, &io) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_dbi failed");
        return RV9_IO_ERR_IO;
    }

    /*
     * The framebuffer is the DPI driver's, allocated from PSRAM because
     * 1.2 MB cannot come from anywhere else on this board. One buffer, not
     * two: a second costs another 1.2 MB and buys tear-free full-screen
     * animation, which nothing here does yet.
     *
     * The 2D-engine copy path is deliberately NOT registered. With it, a
     * draw hands the copy to the engine and returns while the engine is
     * still reading the caller's buffer -- and every caller above this
     * paints into one band buffer and reuses it immediately. That is the
     * exact bug panel.c carries a long comment about having fixed on the
     * C5. Left alone, the copy happens in the caller's own context and the
     * buffer is its own again when the call returns. In IDF v6 the engine
     * is opt-in through a hook registration rather than a config flag, so
     * not asking is all it takes. The faster path needs the completion
     * plumbing panel.c already demonstrates, and is an optimisation for
     * when something needs it.
     */
    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel    = 0,
        .dpi_clock_freq_mhz = 52,     /* dpi_clk_src: zero, as above */
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = PANEL_W,
            .v_size            = PANEL_H,
            .hsync_pulse_width = 10,
            .hsync_back_porch  = 160,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 1,
            .vsync_back_porch  = 23,
            .vsync_front_porch = 12,
        },
    };
    if (esp_lcd_new_panel_dpi(bus, &dpi, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_dpi failed (framebuffer is "
                      "%d KB and wants PSRAM)",
                 PANEL_W * PANEL_H * 2 / 1024);
        return RV9_IO_ERR_IO;
    }

    /* Hardware reset, then the maker's sequence, then start the video.
       This is the order the vendor driver uses and the panel wants it. */
    gpio_config_t rst = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_RST,
    };
    if (gpio_config(&rst) != ESP_OK) return RV9_IO_ERR_IO;

    gpio_set_level(PIN_RST, 0);
    rv9_task_delay_ms(10);
    gpio_set_level(PIN_RST, 1);
    rv9_task_delay_ms(20);

    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        const init_cmd_t *c = &s_init[i];
        esp_err_t e = esp_lcd_panel_io_tx_param(io, c->cmd,
                                                c->has_data ? &c->data : NULL,
                                                c->has_data ? 1 : 0);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "init command %02x failed", c->cmd);
            return RV9_IO_ERR_IO;
        }
        if (c->delay_ms) rv9_task_delay_ms((uint32_t)c->delay_ms);
    }

    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed");
        return RV9_IO_ERR_IO;
    }

    s_w = PANEL_W;
    s_h = PANEL_H;
    s_up = true;

    if (w) *w = s_w;
    if (h) *h = s_h;

    ESP_LOGI(TAG, "EK79007 up: %dx%d, %d lanes at %d Mbps, %d KB framebuffer",
             s_w, s_h, DSI_LANES, DSI_LANE_MBPS, PANEL_W * PANEL_H * 2 / 1024);
    return RV9_IO_OK;
}

void rv9_panel_size(int *w, int *h)
{
    if (w) *w = s_w;
    if (h) *h = s_h;
}

void rv9_panel_physical(uint32_t *w_um, uint32_t *h_um, uint8_t *kind)
{
    /* No swap, unlike the C5: this panel is native landscape and cannot be
       turned, so the millimetres always go the same way as the pixels. */
    if (w_um) *w_um = PANEL_W_UM;
    if (h_um) *h_um = PANEL_H_UM;
    if (kind) *kind = RV9_PHYS_FIXED;
}

bool rv9_panel_take(const void *owner)
{
    if (s_owner == owner) return false;
    s_owner = owner;
    return true;
}

bool rv9_panel_is_owner(const void *owner)
{
    return s_owner == owner;
}

/*
 * Put pixels on the glass.
 *
 * Shorter than panel.c's because there is nothing to wait for: with
 * use_dma2d off the copy into the framebuffer happens here, in this
 * context, and the caller's buffer is its own again when this returns. The
 * scan-out reads the framebuffer, not the caller.
 *
 * The lock is still needed. Two devices drawing at once would interleave
 * their copies into the same framebuffer, which is the same reason panel.c
 * holds one.
 */
void rv9_panel_blit(int x0, int y0, int x1, int y1, const uint16_t *px)
{
    if (!s_up || px == NULL) return;
    if (x1 <= x0 || y1 <= y0) return;

    rv9_lock_acquire(s_lock);

    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, px) != ESP_OK) {
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "a panel draw was refused");
        }
    }

    rv9_lock_release(s_lock);
}
