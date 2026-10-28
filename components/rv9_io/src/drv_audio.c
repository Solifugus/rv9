/*
 * audio driver -- the P4 board's two microphones and its speaker, as /au0.
 *
 * Three chips on one I2S bus, all clocked by the P4:
 *
 *   ES7210 at 0x40   a four-channel microphone converter. MIC1 and MIC2
 *                    are the two capsules printed on the board; it sends
 *                    them as the left and right of each frame on DSIN.
 *   ES8311 at 0x18   a mono codec. Only its DAC is used: DOUT goes in,
 *                    and its one output drives the speaker
 *   NS4150B          the speaker amplifier, switched by GPIO 53. Nothing
 *                    else turns it on, so an idle device makes no hiss.
 *
 * The pins and the two addresses are Waveshare's (their board support
 * package for the ESP32-P4-WIFI6-Touch-LCD-7B, 3.0.1), and the register
 * sequences follow Espressif's esp_codec_dev 1.5 for the same two chips,
 * as slaves with MCLK from the pin at 256 x fs. They are written out here
 * rather than linked: a few dozen register writes are not worth a second
 * device model underneath this one.
 *
 * The format is fixed: 16 kHz, two channels, 16-bit. 16 kHz because it is
 * what speech recognition wants and what the bus can share between the
 * two directions -- they have one clock. Two channels because that is what
 * a frame on the wire is: in, MIC1 and MIC2; out, the ES8311 plays the
 * left and ignores the right. A program that has one channel to say
 * duplicates it, which costs it a loop and keeps this driver honest about
 * what the hardware does.
 *
 * NOTHING HERE BLOCKS THE HOST TASK. Every RV-9 thread runs inside one
 * host task, so an I2S call that waited would stop all of them. Reads and
 * writes ask the DMA queue with a zero timeout; a read with nothing yet
 * says so and SCF sleeps on the caller's behalf, and a write with no room
 * sleeps here, a few milliseconds at a time, through rv9_task_delay_ms --
 * which lets the others run.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include "i2c_bus.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-audio";

#if CONFIG_IDF_TARGET_ESP32P4

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#define PIN_MCLK   13
#define PIN_BCLK   12
#define PIN_WS     10
#define PIN_DOUT    9     /* to the ES8311 */
#define PIN_DIN    11     /* from the ES7210 */
#define PIN_PA     53     /* the amplifier's enable */

#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x40

#define RATE        16000
#define FRAME_BYTES 4     /* two 16-bit samples */

/* 6 x 320 frames is 120 ms each way: room for a reader that is late by a
   scheduler tick or two, and 15 KB of internal memory while open. */
#define DMA_DESCS   6
#define DMA_FRAMES  320

#define DEFAULT_VOLUME_DB  -10   /* the speaker is small and close */
#define DEFAULT_GAIN_DB     30   /* what Espressif's own driver starts at */

typedef struct {
    i2c_master_dev_handle_t dac;        /* ES8311 */
    i2c_master_dev_handle_t adc;        /* ES7210 */
    i2s_chan_handle_t       tx, rx;
    rv9_lock_t              lock;
    bool                    running;
    int32_t                 volume_db;
    uint32_t                gain_db;
} audio_t;

/* ---- registers ---- */

static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, 2, 50);
}

static uint8_t rd(i2c_master_dev_handle_t d, uint8_t reg)
{
    uint8_t v = 0;
    (void)i2c_master_transmit_receive(d, &reg, 1, &v, 1, 50);
    return v;
}

static esp_err_t upd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t mask,
                     uint8_t val)
{
    return wr(d, reg, (uint8_t)((rd(d, reg) & ~mask) | (val & mask)));
}

/* ---- ES8311: the speaker ---- */

/* 0.5 dB a step from -95.5 at 0, so 0 dB is 191. */
static uint8_t volume_reg(int32_t db)
{
    int32_t v = 191 + 2 * db;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static esp_err_t es8311_start(audio_t *a)
{
    i2c_master_dev_handle_t d = a->dac;
    esp_err_t e = ESP_OK;

    if (rd(d, 0x0D) != 0xFA) e |= wr(d, 0x0D, 0xFA);
    /* Twice: the first write after power-up is known to go missing. */
    e |= wr(d, 0x44, 0x08);
    e |= wr(d, 0x44, 0x08);

    e |= wr(d, 0x01, 0x30);
    e |= wr(d, 0x02, 0x00);
    e |= wr(d, 0x03, 0x10);
    e |= wr(d, 0x16, 0x24);
    e |= wr(d, 0x04, 0x10);
    e |= wr(d, 0x05, 0x00);
    e |= wr(d, 0x0B, 0x00);
    e |= wr(d, 0x0C, 0x00);
    e |= wr(d, 0x10, 0x1F);
    e |= wr(d, 0x11, 0x7F);
    e |= wr(d, 0x00, 0x80);                 /* powered, slave */
    e |= wr(d, 0x01, 0x3F);                 /* clocks on, from the MCLK pin */
    e |= upd(d, 0x06, 0x20, 0x00);          /* SCLK not inverted */
    e |= wr(d, 0x13, 0x10);
    e |= wr(d, 0x1B, 0x0A);
    e |= wr(d, 0x1C, 0x6A);
    e |= wr(d, 0x44, 0x58);

    /* 16-bit standard I2S, both directions. */
    e |= wr(d, 0x09, (uint8_t)((rd(d, 0x09) | 0x0C) & 0xFC));
    e |= wr(d, 0x0A, (uint8_t)((rd(d, 0x0A) | 0x0C) & 0xFC));

    /* 4.096 MHz MCLK at 16 kHz: no pre-divide, no multiply, one to one. */
    e |= upd(d, 0x02, 0xF8, 0x00);
    e |= wr(d, 0x05, 0x00);
    e |= upd(d, 0x03, 0x7F, 0x10);
    e |= upd(d, 0x04, 0x7F, 0x20);
    e |= upd(d, 0x07, 0x3F, 0x00);
    e |= wr(d, 0x08, 0xFF);
    e |= upd(d, 0x06, 0x1F, 0x03);

    /* Start the DAC. */
    e |= upd(d, 0x09, 0x40, 0x00);
    e |= upd(d, 0x0A, 0x40, 0x00);
    e |= wr(d, 0x17, 0xBF);
    e |= wr(d, 0x0E, 0x02);
    e |= wr(d, 0x12, 0x00);
    e |= wr(d, 0x14, 0x1A);
    e |= wr(d, 0x0D, 0x01);
    e |= wr(d, 0x15, 0x40);
    e |= wr(d, 0x37, 0x08);
    e |= wr(d, 0x45, 0x00);

    e |= wr(d, 0x32, volume_reg(a->volume_db));
    e |= upd(d, 0x31, 0x60, 0x00);          /* unmute */
    return e;
}

static void es8311_stop(audio_t *a)
{
    i2c_master_dev_handle_t d = a->dac;
    upd(d, 0x31, 0x60, 0x60);               /* mute first: no click */
    wr(d, 0x32, 0x00);
    wr(d, 0x17, 0x00);
    wr(d, 0x0E, 0xFF);
    wr(d, 0x12, 0x02);
    wr(d, 0x14, 0x00);
    wr(d, 0x0D, 0xFA);
    wr(d, 0x15, 0x00);
    wr(d, 0x02, 0x10);
    wr(d, 0x00, 0x00);
    wr(d, 0x00, 0x1F);
    wr(d, 0x01, 0x30);
    wr(d, 0x01, 0x00);
    wr(d, 0x45, 0x00);
    wr(d, 0x0D, 0xFC);
    wr(d, 0x02, 0x00);
}

/* ---- ES7210: the microphones ---- */

/* 3 dB a step to 33, then 34.5, 36 and 37.5. */
static uint8_t gain_code(uint32_t db)
{
    if (db < 34) return (uint8_t)(db / 3);
    if (db < 36) return 12;
    if (db < 37) return 13;
    return 14;
}

static esp_err_t es7210_gain(audio_t *a)
{
    uint8_t g = gain_code(a->gain_db);
    return upd(a->adc, 0x43, 0x0F, g) | upd(a->adc, 0x44, 0x0F, g);
}

/* MIC1 and MIC2 on, 3 and 4 off: two capsules, standard I2S, no TDM. */
static esp_err_t es7210_mics(audio_t *a)
{
    i2c_master_dev_handle_t d = a->adc;
    esp_err_t e = ESP_OK;
    for (uint8_t r = 0x43; r <= 0x46; r++) e |= upd(d, r, 0x10, 0x00);
    e |= wr(d, 0x4B, 0xFF);
    e |= wr(d, 0x4C, 0xFF);
    e |= upd(d, 0x01, 0x0B, 0x00);
    e |= wr(d, 0x4B, 0x00);
    e |= upd(d, 0x43, 0x10, 0x10);
    e |= upd(d, 0x44, 0x10, 0x10);
    e |= es7210_gain(a);
    e |= wr(d, 0x12, 0x00);
    return e;
}

static esp_err_t es7210_start(audio_t *a)
{
    i2c_master_dev_handle_t d = a->adc;
    esp_err_t e = ESP_OK;

    e |= wr(d, 0x00, 0xFF);                 /* reset */
    e |= wr(d, 0x00, 0x41);
    e |= wr(d, 0x01, 0x3F);
    e |= wr(d, 0x09, 0x30);
    e |= wr(d, 0x0A, 0x30);
    e |= wr(d, 0x23, 0x2A);                 /* high-pass filters */
    e |= wr(d, 0x22, 0x0A);
    e |= wr(d, 0x20, 0x0A);
    e |= wr(d, 0x21, 0x2A);
    e |= upd(d, 0x08, 0x01, 0x00);          /* slave */
    e |= wr(d, 0x40, 0x43);
    e |= wr(d, 0x41, 0x70);                 /* mic bias 2.87 V */
    e |= wr(d, 0x42, 0x70);
    e |= wr(d, 0x07, 0x20);
    e |= wr(d, 0x02, 0xC1);
    e |= es7210_mics(a);
    uint8_t clocks = rd(d, 0x01);

    e |= wr(d, 0x11, (uint8_t)(((rd(d, 0x11) & 0x1F) | 0x60) & 0xFC));

    /* Power it up. */
    e |= wr(d, 0x01, clocks);
    e |= wr(d, 0x06, 0x00);
    e |= wr(d, 0x40, 0x43);
    for (uint8_t r = 0x47; r <= 0x4A; r++) e |= wr(d, r, 0x08);
    e |= es7210_mics(a);
    e |= wr(d, 0x40, 0x43);
    e |= wr(d, 0x00, 0x71);
    e |= wr(d, 0x00, 0x41);
    return e;
}

static void es7210_stop(audio_t *a)
{
    i2c_master_dev_handle_t d = a->adc;
    for (uint8_t r = 0x47; r <= 0x4C; r++) wr(d, r, 0xFF);
    wr(d, 0x40, 0xC0);
    wr(d, 0x01, 0x7F);
    wr(d, 0x06, 0x07);
}

/* ---- the bus ---- */

static void bus_free(audio_t *a)
{
    if (a->tx) { i2s_channel_disable(a->tx); i2s_del_channel(a->tx); }
    if (a->rx) { i2s_channel_disable(a->rx); i2s_del_channel(a->rx); }
    a->tx = a->rx = NULL;
}

/* The clocks first: both chips are slaves, and the ES8311 counts its own
   from MCLK. Then the chips, then the amplifier, last, so it never
   amplifies a DAC that is still settling. */
static rv9_io_err_t start_locked(audio_t *a)
{
    if (a->running) return RV9_IO_OK;

    /* ESP-IDF warns, on every start, that the receive half has been made
       the transmit half's slave. That is what full duplex on one bus is,
       and the arrangement asked for; a warning each time somebody records
       is noise in a log people read for faults. Here rather than at init,
       because main.c sets every tag at once after boot, which forgets it. */
    esp_log_level_set("i2s_common", ESP_LOG_ERROR);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                      I2S_ROLE_MASTER);
    cc.dma_desc_num  = DMA_DESCS;
    cc.dma_frame_num = DMA_FRAMES;
    cc.auto_clear    = true;        /* an empty queue plays silence */
    if (i2s_new_channel(&cc, &a->tx, &a->rx) != ESP_OK) {
        ESP_LOGE(TAG, "no I2S channel");
        a->tx = a->rx = NULL;
        return RV9_IO_ERR_NOMEM;
    }

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS,
            .dout = PIN_DOUT, .din  = PIN_DIN,
        },
    };
    if (i2s_channel_init_std_mode(a->tx, &sc) != ESP_OK ||
        i2s_channel_init_std_mode(a->rx, &sc) != ESP_OK ||
        i2s_channel_enable(a->tx) != ESP_OK ||
        i2s_channel_enable(a->rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S would not start");
        bus_free(a);
        return RV9_IO_ERR_IO;
    }

    if (es8311_start(a) != ESP_OK || es7210_start(a) != ESP_OK) {
        ESP_LOGE(TAG, "a codec did not take its settings");
        es8311_stop(a);
        es7210_stop(a);
        bus_free(a);
        return RV9_IO_ERR_IO;
    }

    gpio_set_level(PIN_PA, 1);
    a->running = true;
    return RV9_IO_OK;
}

static void stop_locked(audio_t *a)
{
    if (!a->running) return;
    gpio_set_level(PIN_PA, 0);
    es8311_stop(a);
    es7210_stop(a);
    bus_free(a);
    a->running = false;
}

static rv9_io_err_t ensure_running(audio_t *a)
{
    rv9_lock_acquire(a->lock);
    rv9_io_err_t err = start_locked(a);
    rv9_lock_release(a->lock);
    return err;
}

/* ---- the driver ---- */

static rv9_io_err_t audio_init(rv9_dev_t *dev)
{
    i2c_master_bus_handle_t bus;
    rv9_io_err_t err = rv9_i2c_bus_claim(0, &bus);
    if (err != RV9_IO_OK) return err;

    /* Both or nothing: a speaker with no microphones, or the reverse, is
       not this board and these pins mean something else. */
    if (i2c_master_probe(bus, ES8311_ADDR, 50) != ESP_OK ||
        i2c_master_probe(bus, ES7210_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "%s: no codecs on the bus", dev->name);
        return RV9_IO_ERR_NOTFOUND;
    }

    audio_t *a = rv9_calloc(1, sizeof(*a));
    if (a == NULL) return RV9_IO_ERR_NOMEM;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz    = 100 * 1000,
    };
    cfg.device_address = ES8311_ADDR;
    bool ok = i2c_master_bus_add_device(bus, &cfg, &a->dac) == ESP_OK;
    cfg.device_address = ES7210_ADDR;
    ok = ok && i2c_master_bus_add_device(bus, &cfg, &a->adc) == ESP_OK;

    /* The chips saying they are the chips: an address answering proves
       only that something is there. */
    uint8_t id8311 = ok ? rd(a->dac, 0xFD) : 0;
    uint8_t id7210 = ok ? rd(a->adc, 0x3D) : 0;
    if (!ok || id8311 != 0x83 || id7210 != 0x72) {
        ESP_LOGE(TAG, "%s: 0x18 says %02x and 0x40 says %02x; expected an "
                      "ES8311 and an ES7210", dev->name, id8311, id7210);
        if (a->dac) i2c_master_bus_rm_device(a->dac);
        if (a->adc) i2c_master_bus_rm_device(a->adc);
        rv9_free(a);
        return RV9_IO_ERR_NOTFOUND;
    }

    if (rv9_lock_create(&a->lock) != RV9_OK) {
        i2c_master_bus_rm_device(a->dac);
        i2c_master_bus_rm_device(a->adc);
        rv9_free(a);
        return RV9_IO_ERR_NOMEM;
    }

    /* Off until somebody uses it. */
    gpio_config_t pa = { .mode = GPIO_MODE_OUTPUT,
                         .pin_bit_mask = 1ULL << PIN_PA };
    gpio_config(&pa);
    gpio_set_level(PIN_PA, 0);

    a->volume_db  = DEFAULT_VOLUME_DB;
    a->gain_db    = DEFAULT_GAIN_DB;
    dev->drv_state = a;

    ESP_LOGI(TAG, "%s: ES7210 microphones and ES8311 speaker, %d Hz, "
                  "2 x 16-bit", dev->name, RATE);
    return RV9_IO_OK;
}

/* The last path has closed: stop the clocks and the amplifier. */
static rv9_io_err_t audio_close(rv9_dev_t *dev)
{
    audio_t *a = (audio_t *)dev->drv_state;
    if (a == NULL) return RV9_IO_OK;
    rv9_lock_acquire(a->lock);
    stop_locked(a);
    rv9_lock_release(a->lock);
    return RV9_IO_OK;
}

static rv9_io_err_t audio_read(rv9_dev_t *dev, void *buf, size_t len,
                               size_t *done)
{
    audio_t *a = (audio_t *)dev->drv_state;
    if (done) *done = 0;
    if (a == NULL || buf == NULL) return RV9_IO_ERR_IO;

    len -= len % FRAME_BYTES;
    if (len == 0) return RV9_IO_ERR_INVAL;      /* not even one frame */

    rv9_io_err_t err = ensure_running(a);
    if (err != RV9_IO_OK) return err;

    size_t got = 0;
    (void)i2s_channel_read(a->rx, buf, len, &got, 0);
    got -= got % FRAME_BYTES;
    if (got == 0) return RV9_IO_ERR_WOULDBLOCK;     /* SCF sleeps and asks again */

    if (done) *done = got;
    return RV9_IO_OK;
}

static rv9_io_err_t audio_write(rv9_dev_t *dev, const void *buf, size_t len,
                                size_t *done)
{
    audio_t *a = (audio_t *)dev->drv_state;
    if (done) *done = 0;
    if (a == NULL || buf == NULL) return RV9_IO_ERR_IO;

    len -= len % FRAME_BYTES;
    rv9_io_err_t err = ensure_running(a);
    if (err != RV9_IO_OK) return err;

    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        size_t put = 0;
        (void)i2s_channel_write(a->tx, p, left, &put, 0);
        p    += put;
        left -= put;
        if (left > 0 && put == 0) rv9_task_delay_ms(5);
    }

    if (done) *done = len;
    return RV9_IO_OK;
}

static rv9_io_err_t audio_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    (void)dev;
    if (code != RV9_AU_GS_FORMAT) return RV9_IO_ERR_UNSUPPORTED;
    if (arg == NULL) return RV9_IO_ERR_INVAL;

    rv9_audio_format_t f = {
        .rate = RATE, .channels = 2, .bits = 16,
        .in_channels = 2, .out_channels = 1,
    };
    memcpy(arg, &f, sizeof(f));
    return RV9_IO_OK;
}

static rv9_io_err_t audio_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    audio_t *a = (audio_t *)dev->drv_state;
    if (a == NULL) return RV9_IO_ERR_IO;
    if (arg == NULL) return RV9_IO_ERR_INVAL;

    rv9_io_err_t err = RV9_IO_OK;
    rv9_lock_acquire(a->lock);
    if (code == RV9_AU_SS_VOLUME) {
        int32_t db = *(int32_t *)arg;
        a->volume_db = db < -95 ? -95 : db > 32 ? 32 : db;
        if (a->running) wr(a->dac, 0x32, volume_reg(a->volume_db));
    } else if (code == RV9_AU_SS_GAIN) {
        uint32_t db = *(uint32_t *)arg;
        a->gain_db = db > 37 ? 37 : db;
        if (a->running) es7210_gain(a);
    } else {
        err = RV9_IO_ERR_UNSUPPORTED;
    }
    rv9_lock_release(a->lock);
    return err;
}

#else   /* not the P4: no codecs, and these pins mean something else */

static rv9_io_err_t audio_init(rv9_dev_t *dev)
{
    ESP_LOGI(TAG, "%s: this board has no audio codecs", dev->name);
    return RV9_IO_ERR_NOTFOUND;
}

#define audio_close   NULL
#define audio_read    NULL
#define audio_write   NULL
#define audio_getstat NULL
#define audio_setstat NULL

#endif

static const rv9_driver_t audio = {
    .name       = "audio",
    .raw_stream = true,       /* samples, not lines: see rv9/io.h */
    .init       = audio_init,
    .close      = audio_close,
    .read       = audio_read,
    .write      = audio_write,
    .getstat    = audio_getstat,
    .setstat    = audio_setstat,
};

rv9_io_err_t rv9_drv_audio_register(void)
{
    return rv9_io_register_driver(&audio);
}
