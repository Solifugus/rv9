/*
 * The panel -- one ST7789, shared.
 *
 * There is a single piece of glass on this board and more than one device
 * that wants to draw on it: `/term` is a text console and `/w0` is a
 * graphics window. Neither of them can own the panel, because a driver
 * that owns hardware another driver also needs is a driver that has to
 * know about the other one.
 *
 * So the panel is here instead: brought up once by whoever asks first,
 * handed out as geometry and a blit, and serialised by a lock so two
 * devices cannot interleave halves of an SPI transfer.
 *
 * It is deliberately thin. It knows about pins and an SPI bus and nothing
 * about characters, shapes, colours or what is being drawn -- those belong
 * to the drivers above, which is the same split the I/O design makes
 * everywhere else, applied one level further down than usual because the
 * hardware forced it.
 */
#include "panel.h"

#include "rv9/kal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "rv9-panel";

#define LCD_HOST      RV9_SPI_HOST
#define PIN_SCLK      7
#define PIN_MOSI      6
#define PIN_CS        23
#define PIN_DC        24
#define PIN_RST       26
#define PIN_BL        10

/* The card's data-out, on the bus the display shares. The display never
   reads, so this line exists only for the card -- but it belongs to the
   bus, and the bus is configured once. */
#define PIN_SD_MISO   5

#define PANEL_W       172
#define PANEL_H       320
#define PANEL_GAP     34    /* 172-wide panel centred in the controller's 240 */
#define LCD_CLK_HZ    (40 * 1000 * 1000)

#define BL_CHANNEL    LEDC_CHANNEL_5
#define BL_TIMER      LEDC_TIMER_1
#define BL_DUTY_BITS  LEDC_TIMER_10_BIT
#define BL_FREQ_HZ    5000

/* The largest single transfer any caller will ask for: a full-width strip
   deep enough for a text row or a render band. */
#define MAX_STRIP_ROWS 24

static esp_lcd_panel_handle_t s_panel;
static rv9_lock_t             s_lock;
static int                    s_w, s_h;
static bool                   s_landscape;
static uint8_t                s_brightness = 100;
static bool                   s_up;
static bool                   s_bus_up;
static const void            *s_owner;
static volatile uint32_t      s_done_count;


/* Runs in the SPI interrupt, once per completed colour transfer. */
static bool trans_done(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io; (void)ev; (void)ctx;

    s_done_count++;
    return false;
}

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

/*
 * Bring the panel up, or report what it already is.
 *
 * The first caller decides the orientation, because there is one panel and
 * it can only face one way; a second caller asking for the other one is
 * told the geometry it is actually getting rather than being refused. In
 * practice both descriptors should agree, and disagreeing is worth a log
 * line rather than a failure.
 */
rv9_io_err_t rv9_panel_bus_claim(void)
{
    if (s_bus_up) return RV9_IO_OK;

    /* Sized from the panel's own dimensions rather than the orientation
       chosen at open: the card may claim the bus before any display does,
       and the largest strip either way fits this. */
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PANEL_H * MAX_STRIP_ROWS * (int)sizeof(uint16_t) + 64,
    };
    if (spi_bus_initialize(RV9_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return RV9_IO_ERR_IO;
    }

    s_bus_up = true;
    return RV9_IO_OK;
}

rv9_io_err_t rv9_panel_open(bool landscape, int *w, int *h)
{
    if (s_up) {
        if (landscape != s_landscape) {
            ESP_LOGW(TAG, "already %s; ignoring a request for the other",
                     s_landscape ? "landscape" : "portrait");
        }
        if (w) *w = s_w;
        if (h) *h = s_h;
        return RV9_IO_OK;
    }

    if (rv9_lock_create(&s_lock) != RV9_OK) return RV9_IO_ERR_NOMEM;

    s_landscape = landscape;
    s_w = landscape ? PANEL_H : PANEL_W;
    s_h = landscape ? PANEL_W : PANEL_H;

    /* The backlight is a PWM output, not a switch: see design section 13.
       Brought up dark, so nothing is shown before it has been drawn. */
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

    rv9_io_err_t berr = rv9_panel_bus_claim();
    if (berr != RV9_IO_OK) return berr;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = trans_done,
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
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "st7789 panel failed");
        return RV9_IO_ERR_IO;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    if (landscape) {
        /* Swapping axes swaps which edge the panel's offset applies to. */
        esp_lcd_panel_swap_xy(s_panel, true);
        esp_lcd_panel_mirror(s_panel, false, true);
        esp_lcd_panel_set_gap(s_panel, 0, PANEL_GAP);
    } else {
        esp_lcd_panel_set_gap(s_panel, PANEL_GAP, 0);
    }

    esp_lcd_panel_invert_color(s_panel, true);   /* ST7789 panels are IPS */
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_up = true;
    if (w) *w = s_w;
    if (h) *h = s_h;

    ESP_LOGI(TAG, "ST7789 up: %dx%d, %s", s_w, s_h,
             landscape ? "landscape" : "portrait");
    return RV9_IO_OK;
}

void rv9_panel_size(int *w, int *h)
{
    if (w) *w = s_w;
    if (h) *h = s_h;
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
 * Put pixels on the glass, and wait until they are actually there.
 *
 * esp_lcd *queues* a transfer and returns; the DMA engine reads the
 * caller's buffer afterwards. Every caller here paints into one buffer and
 * reuses it immediately, so returning early means the next band is written
 * over the one still being sent -- horizontal bands of wrong pixels across
 * the picture, and invisible when consecutive blits happen to hold similar
 * content, as a text console's usually do.
 *
 * The wait is a spin on a counter the completion interrupt bumps, rather
 * than a semaphore.
 *
 * A semaphore was the obvious choice and it did nothing: every blit sat
 * out its full timeout, and boot went from 1.2 seconds to fourteen.
 * rv9_sem_give_from_isr is not implemented under the native kernel --
 * making its wait queues interrupt-safe is real work and has not been
 * done -- so it refuses, with RV9_ERR_UNSUPPORTED, in as many words. The
 * stub was honest. This code dropped the return value on the floor.
 *
 * Both ISR-safe KAL primitives now carry RV9_MUST_CHECK so the compiler
 * objects, which is where an unimplemented call should be caught.
 *
 * The counter is the better fit here anyway: there is exactly one waiter,
 * it knows what it is waiting for, and the wait is shorter than a context
 * switch would cost.
 *
 * What is here instead is exact: a full-width five-row strip takes 636 us
 * at 40 MHz, and that is what the spin measures. Burning those microseconds
 * is the cost of the transfer either way -- the SPI has to happen before
 * the buffer is safe to touch. The deadline is only there so a wedged
 * peripheral cannot hang the system.
 */
void rv9_panel_blit(int x0, int y0, int x1, int y1, const uint16_t *px)
{
    if (!s_up || px == NULL) return;
    if (x1 <= x0 || y1 <= y0) return;

    rv9_lock_acquire(s_lock);

    uint32_t want = s_done_count + 1;

    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, px) == ESP_OK) {
        uint64_t deadline = rv9_time_us() + 50000;

        /*
         * Wait, but hand the processor over while waiting.
         *
         * A full-width strip takes 636 us and there are twenty-two of them
         * in a picture, so a spin gives away fourteen milliseconds of CPU
         * per frame to no purpose. On a bench that is invisible; on a
         * machine with a control loop it is fourteen milliseconds somebody
         * else could have used.
         *
         * Yielding rather than sleeping, because the wait is far shorter
         * than any tick and this must also work before the kernel is
         * serving threads -- the console clears the panel at init, from a
         * context that cannot block.
         */
        while (s_done_count != want && rv9_time_us() < deadline) {
            rv9_task_yield();
        }

        if (s_done_count != want) {
            static bool warned;
            if (!warned) {
                warned = true;
                ESP_LOGW(TAG, "a panel transfer never completed");
            }
        }
    }

    rv9_lock_release(s_lock);
}
