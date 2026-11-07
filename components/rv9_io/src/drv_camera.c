/*
 * camera driver -- the P4 board's OV5647, as /cam0.
 *
 * Four pieces of hardware in a line, three of them the P4's own:
 *
 *   OV5647 at 0x36   a five-megapixel sensor on the ribbon under the screen,
 *                    told what to do over the two-wire bus the touch panel
 *                    and the codecs are on. It sends raw Bayer pixels --
 *                    one colour per pixel, green-blue/red-green -- over two
 *                    MIPI lanes. It has its own 24 MHz clock.
 *   CSI              the P4's MIPI receiver, which takes the lanes in.
 *   ISP              its image processor, between the two: it turns one
 *                    colour per pixel into three (RGB565) on the way to
 *                    memory, so nothing here touches a pixel.
 *   JPEG             its compressor, for a picture somebody will keep.
 *
 * The sensor's settings are Espressif's, from esp_cam_sensor 2.6.0 (Apache
 * 2.0, esp-video-components 08bcabd): its 800x640 RAW8 50 fps mode, the
 * auto-exposure target and the banding filter it sets after. They are
 * written out here rather than linked, as the codecs' are in drv_audio.c:
 * a hundred register writes are not worth a second device model, a
 * detection table and a component manager dependency underneath this one.
 * 800x640 because it is the sensor's whole width scaled down -- the widest
 * view it has -- in a megabyte, which is a size a picture can be handled
 * at; the 1920x1080 mode is a crop of the middle and four times the bytes.
 *
 * WHERE THE WORK HAPPENS
 *
 * Not in the caller. Starting the sensor is a hundred and
 * fifty bus writes and then half a second for its exposure to settle;
 * a frame is twenty milliseconds of waiting and a JPEG another few tens.
 * Every RV-9 thread shares one host task, so all of that is done in a
 * host task of the driver's own, below RV-9's (rv9_task_create_background),
 * and a read says "not yet" until it is finished.
 *
 * Handing over is by one word, `state`, as in drv_tts.c: the caller moves
 * it IDLE -> QUEUED, the worker QUEUED -> RUNNING -> DONE or FAILED, and
 * the caller DONE -> IDLE when the last byte has been read. The worker
 * also owns everything to do with the hardware -- starting it, and
 * stopping it when the caller says the last path has gone (`want_stop`)
 * -- so the hardware is only ever touched from one task.
 *
 * WHAT IT HOLDS
 *
 * While open: a frame (1 MB), the CSI driver's own spare frame (another,
 * where pictures nobody asked for go) and room for a JPEG, all external
 * memory. Given back when the last path closes. Nothing at all before the
 * first open.
 *
 * WHITE BALANCE is the one thing the driver does to the picture rather
 * than to the hardware: see awb_step().
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include "i2c_bus.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-cam";

#if CONFIG_IDF_TARGET_ESP32P4

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "driver/isp.h"
#include "driver/isp_awb.h"
#include "driver/isp_wbg.h"
#include "driver/jpeg_encode.h"

#define OV5647_ADDR   0x36
#define OV5647_PID    0x5647

#define WIDTH         800
#define HEIGHT        640
#define FRAME_BYTES   (WIDTH * HEIGHT * 2)
#define JPEG_MAX      (FRAME_BYTES / 2)     /* far more than quality 100 makes */
#define LANE_MBPS     400                   /* 100 MHz pixel clock x 4 */

#define DPHY_LDO_CHAN 3                     /* shared with the DSI panel */
#define DPHY_LDO_MV   2500

#define SETTLE_MS     600   /* auto-exposure, from dark to a lit room */
#define WORKER_STACK  4096
#define DEFAULT_QUALITY 80

#define GAIN_ONE      256   /* the ISP's white balance gains are 8.8 */
#define GAIN_MIN      (GAIN_ONE / 2)
#define GAIN_MAX      (GAIN_ONE * 4)

#define REG_END   0xffff
#define REG_DELAY 0xeeee

typedef struct { uint16_t reg; uint8_t val; } reg_t;

/* ---- the sensor's settings, after esp_cam_sensor's ov5647 ---- */

static const reg_t ov5647_reset[] = {
    {0x0100, 0x00},         /* asleep */
    {0x0103, 0x01},         /* software reset */
    {REG_DELAY, 10},
    {0x4800, 0x01},         /* clock lane off, so it sits in LP-11 */
    {REG_END, 0},
};

/* MIPI_2lane_24Minput_RAW8_800x640_50fps: the full array, binned and
   scaled to 800x640, 8 bits a pixel. The PLL line works out to the
   100 MHz pixel clock CSI is told about below.

   Turned half round from Espressif's: 0x3821 without its mirror bit,
   0x3820 with its flip bit. Mounted on this board -- looking up from
   below the screen -- their settings gave a picture upside down and
   mirrored, measured with a left hand held over the lens. Turning it
   moves the colour filter under the window by a pixel each way, so the
   pattern the ISP is told is GRBG, not Espressif's GBRG -- told the old
   one, red and blue trade places. */
static const reg_t ov5647_800x640[] = {
    {0x3034, 0x18}, {0x3035, 0x41}, {0x3036, 128},  {0x303c, 0x11},
    {0x3106, 0xf5}, {0x3821, 0x01}, {0x3820, 0x43}, {0x3827, 0xec},
    {0x370c, 0x0f}, {0x3612, 0x59}, {0x3618, 0x00}, {0x5000, 0xff},
    {0x583e, 0xf0}, {0x583f, 0x20},                 /* lens shading gains */
    {0x5002, 0x41}, {0x5003, 0x08}, {0x5a00, 0x08}, {0x3000, 0x00},
    {0x3001, 0x00}, {0x3002, 0x00}, {0x3016, 0x08}, {0x3017, 0xe0},
    {0x3018, 0x44}, {0x301c, 0xf8}, {0x301d, 0xf0}, {0x3a18, 0x00},
    {0x3a19, 0xf8}, {0x3c01, 0x80}, {0x3c00, 0x40}, {0x3b07, 0x0c},
    {0x380c, 1896 >> 8}, {0x380d, 1896 & 0xff},     /* line length */
    {0x380e, 984 >> 8},  {0x380f, 984 & 0xff},      /* frame length */
    {0x3814, 0x31}, {0x3815, 0x31}, {0x3708, 0x64}, {0x3709, 0x52},
    {0x3800, 500 >> 8},  {0x3801, 500 & 0xff},      /* window x start */
    {0x3802, 0},         {0x3803, 0},               /* y start */
    {0x3804, 2623 >> 8}, {0x3805, 2623 & 0xff},     /* x end */
    {0x3806, 1953 >> 8}, {0x3807, 1953 & 0xff},     /* y end */
    {0x3808, WIDTH >> 8},  {0x3809, WIDTH & 0xff},  /* output size */
    {0x380a, HEIGHT >> 8}, {0x380b, HEIGHT & 0xff},
    {0x3810, 0}, {0x3811, 8}, {0x3812, 0}, {0x3813, 0},
    {0x3630, 0x2e}, {0x3632, 0xe2}, {0x3633, 0x23}, {0x3634, 0x44},
    {0x3636, 0x06}, {0x3620, 0x64}, {0x3621, 0xe0}, {0x3600, 0x37},
    {0x3704, 0xa0}, {0x3703, 0x5a}, {0x3715, 0x78}, {0x3717, 0x01},
    {0x3731, 0x02}, {0x370b, 0x60}, {0x3705, 0x1a}, {0x3f05, 0x02},
    {0x3f06, 0x10}, {0x3f01, 0x0a}, {0x3a08, 0x01}, {0x3a09, 0x27},
    {0x3a0a, 0x00}, {0x3a0b, 0xf6}, {0x3a0d, 0x04}, {0x3a0e, 0x03},
    {0x3a0f, 0x58}, {0x3a10, 0x50}, {0x3a1b, 0x58}, {0x3a1e, 0x50},
    {0x3a11, 0x60}, {0x3a1f, 0x28}, {0x4001, 0x02}, {0x4004, 0x02},
    {0x4000, 0x09},
    {0x4837, 40},           /* the pixel period, ns, for the MIPI timing */
    {0x4050, 0x6e}, {0x4051, 0x8f},
    {REG_END, 0},
};

enum { ST_IDLE, ST_QUEUED, ST_RUNNING, ST_DONE, ST_FAILED };

typedef struct {
    i2c_master_dev_handle_t sensor;
    esp_ldo_channel_handle_t ldo;
    esp_cam_ctlr_handle_t   csi;
    isp_proc_handle_t       isp;
    jpeg_encoder_handle_t   jpeg;
    isp_awb_ctlr_t          awb;
    bool                    wbg_on;
    isp_wbg_gain_t          gain;       /* white balance, kept across pictures */
    int64_t                 awb_at;     /* when it was last measured */
    uint8_t                *frame;      /* FRAME_BYTES, external */
    uint8_t                *out;        /* JPEG_MAX, external */
    size_t                  out_cap;

    volatile int            state;
    volatile bool           want_stop;
    volatile bool           frame_in;   /* set from the CSI interrupt */
    bool                    started, worker_started;
    int64_t                 started_at;

    uint32_t                format, quality;    /* the caller's */
    const uint8_t          *pic;                /* what reads hand over */
    uint32_t                pic_pos;
    rv9_cam_frame_t         info;
} cam_t;

/* ---- the sensor ---- */

static esp_err_t wr(cam_t *c, uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { reg >> 8, reg & 0xff, val };
    return i2c_master_transmit(c->sensor, b, 3, 50);
}

static esp_err_t rd(cam_t *c, uint16_t reg, uint8_t *val)
{
    uint8_t a[2] = { reg >> 8, reg & 0xff };
    return i2c_master_transmit_receive(c->sensor, a, 2, val, 1, 50);
}

static esp_err_t wr_table(cam_t *c, const reg_t *t)
{
    for (; t->reg != REG_END; t++) {
        if (t->reg == REG_DELAY) {
            rv9_task_delay_ms(t->val);
            continue;
        }
        esp_err_t e = wr(c, t->reg, t->val);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

/* Where auto-exposure aims, and how far off it may be before it moves:
   esp_cam_sensor's arithmetic, at its default target. */
static esp_err_t ae_target(cam_t *c, int target)
{
    int lo = target * 23 / 25, hi = target * 27 / 25;
    int fast_hi = hi * 2 > 255 ? 255 : hi * 2, fast_lo = lo / 2;
    esp_err_t e = wr(c, 0x3a0f, hi);
    e |= wr(c, 0x3a10, lo);
    e |= wr(c, 0x3a1b, hi);
    e |= wr(c, 0x3a1e, lo);
    e |= wr(c, 0x3a11, fast_hi);
    e |= wr(c, 0x3a1f, fast_lo);
    return e;
}

/* The banding filter: exposure in whole cycles of mains flicker, worked
   out from the clocks the table above set. Without it, lights at 50 or
   60 Hz put bars across the picture. */
static esp_err_t banding(cam_t *c)
{
    static const int pre_div2x[] = {2, 2, 4, 6, 8, 3, 12, 5, 16, 2, 2, 2, 2, 2, 2, 2};
    static const int sdiv0[]     = {16, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const int bit_div2x[] = {2, 2, 2, 2, 2, 2, 2, 2, 4, 2, 5, 2, 2, 2, 2, 2};
    static const int sclk_div[]  = {1, 2, 4, 1};
    uint8_t r3037 = 0, r3036 = 0, r3035 = 0, r3034 = 0, r3106 = 0;
    uint8_t h = 0, l = 0;
    rd(c, 0x3037, &r3037);
    rd(c, 0x3036, &r3036);
    rd(c, 0x3035, &r3035);
    rd(c, 0x3034, &r3034);
    rd(c, 0x3106, &r3106);
    int vco = 2400 * 2 / pre_div2x[r3037 & 0x0f] * r3036;   /* 24 MHz in 10 kHz */
    int sysclk = vco * 2 / sdiv0[r3035 >> 4] / ((r3037 & 0x10) ? 2 : 1)
                 / bit_div2x[r3034 & 0x0f] / sclk_div[(r3106 >> 2) & 3];
    rd(c, 0x380c, &h); rd(c, 0x380d, &l);
    int hts = (h << 8) | l;
    rd(c, 0x380e, &h); rd(c, 0x380f, &l);
    int vts = (h << 8) | l;
    if (hts == 0) return ESP_FAIL;

    int step60 = sysclk * 100 / hts * 100 / 120;
    int step50 = sysclk * 100 / hts;
    if (step60 == 0 || step50 == 0) return ESP_FAIL;
    esp_err_t e = wr(c, 0x3a0a, step60 >> 8);
    e |= wr(c, 0x3a0b, step60 & 0xff);
    e |= wr(c, 0x3a0d, (vts - 4) / step60);
    e |= wr(c, 0x3a08, step50 >> 8);
    e |= wr(c, 0x3a09, step50 & 0xff);
    e |= wr(c, 0x3a0e, (vts - 4) / step50);
    return e;
}

/* Non-continuous clock with the lane gated between packets, which is what
   the OV5647 does; bit 0 stops the clock lane altogether. */
static esp_err_t stream(cam_t *c, bool on)
{
    esp_err_t e = wr(c, 0x4800, on ? 0x24 : 0x25);
    e |= wr(c, 0x0100, on ? 0x01 : 0x00);
    return e;
}

/* ---- the worker's side ---- */

static IRAM_ATTR bool frame_done(esp_cam_ctlr_handle_t h,
                                 esp_cam_ctlr_trans_t *t, void *arg)
{
    (void)h; (void)t;
    ((cam_t *)arg)->frame_in = true;
    return false;
}

/*
 * White balance.
 *
 * The ISP counts, over the middle of the picture, the pixels that could be
 * something white under a coloured light -- not too bright to trust, red
 * and blue within a factor of five of green -- and sums their three
 * channels. Grey-world over those: whatever light they are under, they
 * should average grey, so red is scaled by G/R and blue by G/B. The ISP's
 * gain stage applies that to every pixel before the colours are made.
 *
 * The statistics are taken AFTER the gain stage, so what is measured is
 * what is left to correct: each step multiplies the gains by the ratio
 * that remains, half of it at a time so that one odd frame cannot swing
 * the picture. Measured that way round, not assumed: the remaining ratios
 * go to 1.00 while the gains stop moving. From neutral under an
 * incandescent bulb it takes about twenty frames, inside the half second
 * the exposure needs anyway, and the gains are kept for the next picture.
 * The window is counted whole in ordinary light -- every pixel passes the
 * white-patch test -- so in practice this is grey-world.
 */
static bool awb_start(cam_t *c)
{
    esp_isp_awb_config_t ac = {
        .sample_point = ISP_AWB_SAMPLE_POINT_1,
        .window = {
            /* The middle 60% across and 62% down: each side a multiple of
               five, which the ISP's 5x5 grid of sub-windows wants. */
            .top_left  = { 160, 120 },
            .btm_right = { 639, 519 },
        },
        .white_patch = {
            .luminance        = { 0, 220 * 3 },
            .red_green_ratio  = { 0.2f, 3.9f },
            .blue_green_ratio = { 0.2f, 3.9f },
        },
    };
    ac.subwindow = ac.window;
    esp_isp_wbg_config_t wc = { .flags.update_once_configured = true };

    if (esp_isp_new_awb_controller(c->isp, &ac, &c->awb) != ESP_OK) {
        c->awb = NULL;
        return false;
    }
    if (esp_isp_wbg_configure(c->isp, &wc) != ESP_OK ||
        esp_isp_wbg_enable(c->isp) != ESP_OK) {
        return false;
    }
    c->wbg_on = true;
    if (c->gain.gain_g == 0) {
        c->gain = (isp_wbg_gain_t){ GAIN_ONE, GAIN_ONE, GAIN_ONE };
    }
    esp_isp_wbg_set_wb_gain(c->isp, c->gain);
    return esp_isp_awb_controller_enable(c->awb) == ESP_OK;
}

static uint32_t clamp_gain(float g)
{
    return g < GAIN_MIN ? GAIN_MIN : g > GAIN_MAX ? GAIN_MAX : (uint32_t)(g + 0.5f);
}

static void awb_step(cam_t *c)
{
    if (c->awb == NULL || !c->wbg_on) return;
    isp_awb_stat_result_t r;
    if (esp_isp_awb_controller_get_oneshot_statistics(c->awb, 100, &r) != ESP_OK ||
        r.white_patch_num < 100 || r.sum_r == 0 || r.sum_b == 0) {
        return;
    }
    float gr = (float)r.sum_g / r.sum_r, gb = (float)r.sum_g / r.sum_b;
    /* Half the remaining correction, geometrically. */
    c->gain.gain_r = clamp_gain(c->gain.gain_r * (1.0f + gr) / 2.0f);
    c->gain.gain_b = clamp_gain(c->gain.gain_b * (1.0f + gb) / 2.0f);
    esp_isp_wbg_set_wb_gain(c->isp, c->gain);
    ESP_LOGD(TAG, "awb: %lu patches, G/R %.2f G/B %.2f -> gains r %lu b %lu",
             (unsigned long)r.white_patch_num, gr, gb,
             (unsigned long)c->gain.gain_r, (unsigned long)c->gain.gain_b);
}

static void hw_stop(cam_t *c)
{
    if (c->sensor) stream(c, false);
    if (c->csi) {
        esp_cam_ctlr_stop(c->csi);
        esp_cam_ctlr_disable(c->csi);
        esp_cam_ctlr_del(c->csi);
        c->csi = NULL;
    }
    if (c->awb) {
        esp_isp_awb_controller_disable(c->awb);
        esp_isp_del_awb_controller(c->awb);
        c->awb = NULL;
    }
    if (c->wbg_on) {
        esp_isp_wbg_disable(c->isp);
        c->wbg_on = false;
    }
    if (c->isp) {
        esp_isp_disable(c->isp);
        esp_isp_del_processor(c->isp);
        c->isp = NULL;
    }
    if (c->jpeg) {
        jpeg_del_encoder_engine(c->jpeg);
        c->jpeg = NULL;
    }
    if (c->ldo) {
        esp_ldo_release_channel(c->ldo);
        c->ldo = NULL;
    }
    heap_caps_free(c->frame);
    heap_caps_free(c->out);
    c->frame = c->out = NULL;
    c->pic = NULL;
    c->started = false;
}

/*
 * Sensor, then the P4's side of the link, then the sensor streaming --
 * the order esp_video uses, and the one that matters: CSI must be waiting
 * before the first frame arrives or it starts mid-frame.
 */
static bool hw_start(cam_t *c)
{
    int64_t t0 = esp_timer_get_time();

    esp_ldo_channel_config_t ldo = { .chan_id = DPHY_LDO_CHAN,
                                     .voltage_mv = DPHY_LDO_MV };
    if (esp_ldo_acquire_channel(&ldo, &c->ldo) != ESP_OK) {
        ESP_LOGE(TAG, "no LDO channel %d for the CSI PHY", DPHY_LDO_CHAN);
        c->ldo = NULL;
        goto fail;
    }

    if (wr_table(c, ov5647_reset) != ESP_OK ||
        wr_table(c, ov5647_800x640) != ESP_OK ||
        ae_target(c, 0x50) != ESP_OK) {
        ESP_LOGE(TAG, "the sensor did not take its settings");
        goto fail;
    }
    if (banding(c) != ESP_OK) ESP_LOGW(TAG, "no banding filter");
    stream(c, false);

    /* The colour types are what CSI's own converter sees, and it sits
       after the ISP: RGB565 in and out is "pass it through". Telling it
       RAW8 -- what the sensor sends, and what ESP-IDF before 6 wanted
       here -- asks it to convert, which it cannot from RAW. */
    esp_cam_ctlr_csi_config_t cc = {
        .ctlr_id                = 0,
        .h_res                  = WIDTH,
        .v_res                  = HEIGHT,
        .lane_bit_rate_mbps     = LANE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num          = 2,
        .queue_items            = 1,
    };
    if (esp_cam_new_csi_ctlr(&cc, &c->csi) != ESP_OK) {
        ESP_LOGE(TAG, "no CSI controller");
        c->csi = NULL;
        goto fail;
    }
    esp_cam_ctlr_evt_cbs_t cbs = { .on_trans_finished = frame_done };
    if (esp_cam_ctlr_register_event_callbacks(c->csi, &cbs, c) != ESP_OK ||
        esp_cam_ctlr_enable(c->csi) != ESP_OK) {
        ESP_LOGE(TAG, "CSI would not start");
        goto fail;
    }

    esp_isp_processor_cfg_t ic = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .h_res                  = WIDTH,
        .v_res                  = HEIGHT,
        .bayer_order            = COLOR_RAW_ELEMENT_ORDER_GRBG, /* flipped: see the table */
    };
    if (esp_isp_new_processor(&ic, &c->isp) != ESP_OK ||
        esp_isp_enable(c->isp) != ESP_OK) {
        ESP_LOGE(TAG, "the ISP would not start");
        goto fail;
    }
    if (!awb_start(c)) ESP_LOGW(TAG, "no white balance");

    jpeg_encode_engine_cfg_t jc = { .timeout_ms = 200 };
    if (jpeg_new_encoder_engine(&jc, &c->jpeg) != ESP_OK) {
        ESP_LOGE(TAG, "no JPEG encoder");
        c->jpeg = NULL;
        goto fail;
    }

    /* Aligned to a cache line by hand: the CSI driver's own allocator
       (esp_cam_ctlr_alloc_buffer, 6.0.3) does not, and the interrupt that
       hands the buffer to DMA asserts when its cache sync is refused. */
    c->frame = heap_caps_aligned_alloc(128, FRAME_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    jpeg_encode_memory_alloc_cfg_t mc = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    c->out = jpeg_alloc_encoder_mem(JPEG_MAX, &mc, &c->out_cap);
    if (c->frame == NULL || c->out == NULL) {
        ESP_LOGE(TAG, "no external memory for a picture");
        goto fail;
    }

    if (esp_cam_ctlr_start(c->csi) != ESP_OK || stream(c, true) != ESP_OK) {
        ESP_LOGE(TAG, "the camera would not stream");
        goto fail;
    }

    c->started    = true;
    c->started_at = esp_timer_get_time();
    c->info.start_ms = (uint16_t)((c->started_at - t0) / 1000);
    return true;

fail:
    hw_stop(c);
    return false;
}

static bool take(cam_t *c)
{
    /* Exposure finds the light while frames go to the CSI driver's spare
       buffer, unread; counted from when streaming started, so a second
       picture does not wait again. */
    int64_t settled = c->started_at + (int64_t)SETTLE_MS * 1000;
    while (esp_timer_get_time() < settled) awb_step(c);
    /* And the light may have changed -- but not in the 200 ms between two
       frames of a live view, which these two steps used to cost a third of. */
    if (esp_timer_get_time() - c->awb_at > 1000 * 1000) {
        for (int i = 0; i < 2; i++) awb_step(c);
        c->awb_at = esp_timer_get_time();
    }

    int64_t t0 = esp_timer_get_time();
    c->frame_in = false;
    esp_cam_ctlr_trans_t t = { .buffer = c->frame, .buflen = FRAME_BYTES };
    if (esp_cam_ctlr_receive(c->csi, &t, 100) != ESP_OK) {
        ESP_LOGE(TAG, "CSI took no frame buffer");
        return false;
    }
    for (int waited = 0; !c->frame_in; waited += 5) {
        if (waited >= 500) {
            ESP_LOGE(TAG, "no frame from the sensor in 500 ms");
            return false;
        }
        rv9_task_delay_ms(5);
    }
    int64_t t1 = esp_timer_get_time();
    c->info.capture_ms = (uint16_t)((t1 - t0) / 1000);
    c->info.width  = WIDTH;
    c->info.height = HEIGHT;
    c->info.format = (uint8_t)c->format;

    if (c->format == RV9_CAM_FMT_RGB565) {
        c->pic          = c->frame;
        c->info.size    = FRAME_BYTES;
        c->info.quality = 0;
        c->info.encode_ms = 0;
        return true;
    }

    jpeg_encode_cfg_t ec = {
        .width         = WIDTH,
        .height        = HEIGHT,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = c->quality,
    };
    uint32_t n = 0;
    if (jpeg_encoder_process(c->jpeg, &ec, c->frame, FRAME_BYTES,
                             c->out, c->out_cap, &n) != ESP_OK || n == 0) {
        ESP_LOGE(TAG, "the JPEG encoder failed");
        return false;
    }
    c->pic            = c->out;
    c->info.size      = n;
    c->info.quality   = (uint8_t)c->quality;
    c->info.encode_ms = (uint16_t)((esp_timer_get_time() - t1) / 1000);
    return true;
}

static void worker(void *arg)
{
    cam_t *c = (cam_t *)arg;
    for (;;) {
        if (c->state == ST_QUEUED) {
            c->state = ST_RUNNING;
            c->info.start_ms = 0;
            bool ok = (c->started || hw_start(c)) && take(c);
            c->state = ok ? ST_DONE : ST_FAILED;
        } else if (c->want_stop && c->state != ST_RUNNING) {
            /* Cleared first: a close that comes while this runs asks again. */
            c->want_stop = false;
            if (c->started) hw_stop(c);
        }
        rv9_task_delay_ms(10);
    }
}

/* ---- the caller's side ---- */

static rv9_io_err_t cam_init(rv9_dev_t *dev)
{
    i2c_master_bus_handle_t bus;
    rv9_io_err_t err = rv9_i2c_bus_claim(0, &bus);
    if (err != RV9_IO_OK) return err;

    if (i2c_master_probe(bus, OV5647_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "%s: no camera on the bus", dev->name);
        return RV9_IO_ERR_NOTFOUND;
    }

    cam_t *c = rv9_calloc(1, sizeof(*c));
    if (c == NULL) return RV9_IO_ERR_NOMEM;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OV5647_ADDR,
        .scl_speed_hz    = 100 * 1000,
    };
    uint8_t h = 0, l = 0;
    bool ok = i2c_master_bus_add_device(bus, &cfg, &c->sensor) == ESP_OK &&
              rd(c, 0x300a, &h) == ESP_OK && rd(c, 0x300b, &l) == ESP_OK;
    if (!ok || ((h << 8) | l) != OV5647_PID) {
        ESP_LOGE(TAG, "%s: 0x36 says %02x%02x; expected an OV5647",
                 dev->name, h, l);
        if (c->sensor) i2c_master_bus_rm_device(c->sensor);
        rv9_free(c);
        return RV9_IO_ERR_NOTFOUND;
    }

    c->format  = RV9_CAM_FMT_JPEG;
    c->quality = DEFAULT_QUALITY;
    dev->drv_state = c;
    ESP_LOGI(TAG, "%s: OV5647, %dx%d; started on first use",
             dev->name, WIDTH, HEIGHT);
    return RV9_IO_OK;
}

static rv9_io_err_t cam_open(rv9_dev_t *dev, uint32_t mode)
{
    (void)mode;
    cam_t *c = (cam_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;

    if (!c->worker_started) {
        if (rv9_task_create_background(worker, "rv9-cam", WORKER_STACK,
                                       c) != RV9_OK) {
            return RV9_IO_ERR_NOMEM;
        }
        c->worker_started = true;
    }
    c->want_stop = false;
    if (c->state != ST_QUEUED && c->state != ST_RUNNING) c->state = ST_IDLE;
    c->format  = RV9_CAM_FMT_JPEG;
    c->quality = DEFAULT_QUALITY;
    return RV9_IO_OK;
}

/* The last path has gone: the worker stops the sensor and frees. */
static rv9_io_err_t cam_close(rv9_dev_t *dev)
{
    cam_t *c = (cam_t *)dev->drv_state;
    if (c != NULL) c->want_stop = true;
    return RV9_IO_OK;
}

static rv9_io_err_t cam_read(rv9_dev_t *dev, void *buf, size_t len,
                             size_t *done)
{
    cam_t *c = (cam_t *)dev->drv_state;
    if (done) *done = 0;
    if (c == NULL || buf == NULL || len == 0) return RV9_IO_ERR_IO;

    switch (c->state) {
    case ST_IDLE:
        c->pic_pos = 0;
        c->state = ST_QUEUED;
        return RV9_IO_ERR_WOULDBLOCK;       /* SCF sleeps and asks again */
    case ST_QUEUED:
    case ST_RUNNING:
        return RV9_IO_ERR_WOULDBLOCK;
    case ST_FAILED:
        c->state = ST_IDLE;
        return RV9_IO_ERR_IO;
    default:
        break;
    }

    uint32_t left = c->info.size - c->pic_pos;
    uint32_t k = left < len ? left : (uint32_t)len;
    memcpy(buf, c->pic + c->pic_pos, k);
    c->pic_pos += k;
    if (c->pic_pos >= c->info.size) c->state = ST_IDLE;
    if (done) *done = k;
    return RV9_IO_OK;
}

static rv9_io_err_t cam_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    cam_t *c = (cam_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;
    if (code != RV9_CAM_GS_FRAME) return RV9_IO_ERR_UNSUPPORTED;
    if (arg == NULL) return RV9_IO_ERR_INVAL;
    if (c->state == ST_QUEUED || c->state == ST_RUNNING) return RV9_IO_ERR_BUSY;
    memcpy(arg, &c->info, sizeof(c->info));
    return RV9_IO_OK;
}

static rv9_io_err_t cam_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    cam_t *c = (cam_t *)dev->drv_state;
    if (c == NULL) return RV9_IO_ERR_IO;
    if (arg == NULL) return RV9_IO_ERR_INVAL;
    if (c->state == ST_QUEUED || c->state == ST_RUNNING) return RV9_IO_ERR_BUSY;

    uint32_t v = *(uint32_t *)arg;
    if (code == RV9_CAM_SS_FORMAT) {
        if (v != RV9_CAM_FMT_JPEG && v != RV9_CAM_FMT_RGB565) {
            return RV9_IO_ERR_INVAL;
        }
        c->format = v;
        return RV9_IO_OK;
    }
    if (code == RV9_CAM_SS_QUALITY) {
        if (v < 1 || v > 100) return RV9_IO_ERR_INVAL;
        c->quality = v;
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

#else   /* not the P4: no camera interface */

static rv9_io_err_t cam_init(rv9_dev_t *dev)
{
    ESP_LOGI(TAG, "%s: this board has no camera interface", dev->name);
    return RV9_IO_ERR_NOTFOUND;
}

#define cam_open    NULL
#define cam_close   NULL
#define cam_read    NULL
#define cam_getstat NULL
#define cam_setstat NULL

#endif

static const rv9_driver_t camera = {
    .name       = "camera",
    .raw_stream = true,       /* pictures, not lines */
    .init       = cam_init,
    .open       = cam_open,
    .close      = cam_close,
    .read       = cam_read,
    .getstat    = cam_getstat,
    .setstat    = cam_setstat,
};

rv9_io_err_t rv9_drv_camera_register(void)
{
    return rv9_io_register_driver(&camera);
}
