/*
 * stt driver -- Whistle speech recognition, as /stt0.
 *
 * Audio goes in by write, the end of the utterance is said with a setstat,
 * and the words come out by read. See RV9_STT_* in rv9/module.h for what a
 * program sees; this is how it is done.
 *
 * WHERE THE WORK HAPPENS
 *
 * Not in the caller. Recognising a sentence takes seconds of arithmetic,
 * and every RV-9 thread shares one host task: done inside a read, it would
 * stop the shell, the network and every other process for as long as it
 * took. So the engine has its own host task, below all of RV-9's
 * (rv9_task_create_background), and the caller's read says "not yet" until
 * it is finished -- which SCF turns into a sleep, which lets everybody else
 * run. The P4 runs FreeRTOS on one core here, so this is a matter of
 * priority rather than of a second processor: the engine gets what RV-9
 * does not want, which on a machine waiting for people is most of it.
 *
 * The engine's PIE kernel uses vector registers RV-9's context switch does
 * not save. That is safe because the engine is not an RV-9 thread: it is
 * switched by the host, whose coprocessor handling covers PIE as it covers
 * the FPU.
 *
 * WHAT IT HOLDS
 *
 * The model, 17 MB of external memory, from first use until the board
 * restarts: reading it again costs seconds, and nothing else on the board
 * wants that much PSRAM. The audio buffer, 30 seconds of it, from first
 * open. Both are the driver's, charged to nobody: this is the prototype
 * arrangement in docs/rv9-voice-modules-design.md, an engine linked into
 * the firmware behind a device, and an engine of its own as a module with
 * an arena is the version after this one.
 *
 * Handing over is by one word, `state`, written by one side at a time:
 * the caller moves it IDLE -> QUEUED, the engine QUEUED -> RUNNING ->
 * DONE or FAILED, the caller DONE/FAILED -> IDLE when it has read the
 * answer. Nothing else is shared while the engine has it.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/stt.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-stt";

#if CONFIG_IDF_TARGET_ESP32P4

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nd_whistle.h"

#define RATE          16000
#define MAX_SECONDS   30
#define CAP_SAMPLES   (RATE * MAX_SECONDS)
#define DEPTH         4           /* of 8 decoder layers: the measured setting */
#define MAX_TOKENS    96
#define TEXT_MAX      512
#define READ_CHUNK    (64 * 1024)
#define ENGINE_STACK  (48 * 1024) /* what the benchmark ran it in */

#define DEFAULT_MODEL "/sd0/whistle.cact"

enum { ST_IDLE, ST_QUEUED, ST_RUNNING, ST_DONE, ST_FAILED };

typedef struct {
    rv9_dev_t       *dev;
    volatile int     state;

    int16_t         *pcm;           /* PSRAM, CAP_SAMPLES */
    uint32_t         n;             /* samples written so far */

    char             model[48];
    bool             loaded, engine_started;
    uint8_t         *blob;
    nd_whistle      *w;

    char             text[TEXT_MAX];
    uint32_t         text_len, text_pos;
    rv9_stt_timing_t timing;
} stt_t;

static void *psram(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

/* See nd_rv9.h. */
void *rv9_stt_alloc_fast(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : psram(n);
}

void rv9_stt_free(void *p) { heap_caps_free(p); }

/* See nd_rv9_tok.h. */
void *rv9_stt_calloc_ext(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
}

static void say_memory(const char *when)
{
    ESP_LOGI(TAG, "%s: internal %u free, largest %u; external %u free",
             when, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- the engine's side ---- */

/* As the system, from whatever volume the path names. */
static bool load_model(stt_t *s)
{
    int64_t t0 = esp_timer_get_time();
    say_memory("loading");

    int p = rv9_io_open(s->model, RV9_MODE_READ);
    if (p < 0) {
        ESP_LOGE(TAG, "%s: cannot open the model", s->model);
        return false;
    }
    uint64_t size = 0;
    if (rv9_io_getstat(p, RV9_GS_SIZE, &size) != RV9_IO_OK || size == 0) {
        ESP_LOGE(TAG, "%s: cannot tell how big it is", s->model);
        rv9_io_close(p);
        return false;
    }

    s->blob = psram((size_t)size);
    if (s->blob == NULL) {
        ESP_LOGE(TAG, "no %lu bytes of external memory for the model",
                 (unsigned long)size);
        rv9_io_close(p);
        return false;
    }

    uint64_t have = 0;
    while (have < size) {
        size_t want = (size - have) < READ_CHUNK ? (size_t)(size - have)
                                                 : READ_CHUNK;
        size_t got = 0;
        if (rv9_io_read(p, s->blob + have, want, &got) != RV9_IO_OK ||
            got == 0) {
            break;
        }
        have += got;
    }
    rv9_io_close(p);
    if (have != size) {
        ESP_LOGE(TAG, "%s: read %lu of %lu bytes", s->model,
                 (unsigned long)have, (unsigned long)size);
        goto fail;
    }

    s->w = heap_caps_calloc(1, sizeof(*s->w),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s->w == NULL) goto fail;
    int rc = nd_whistle_open(s->w, s->blob, (size_t)size);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s is not a Whistle model this engine reads (%d)",
                 s->model, rc);
        goto fail;
    }
    nd_whistle_set_depth(s->w, DEPTH);
    nd_whistle_cache_head(s->w, psram);

    s->timing.load_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%s: %lu bytes in %lu ms, %lu of %lu decoder layers",
             s->model, (unsigned long)size, (unsigned long)s->timing.load_ms,
             (unsigned long)s->w->depth, (unsigned long)s->w->dec_layers);
    s->loaded = true;
    return true;

fail:
    heap_caps_free(s->w);
    s->w = NULL;
    heap_caps_free(s->blob);
    s->blob = NULL;
    return false;
}

static bool recognise(stt_t *s)
{
    say_memory("before");

    uint32_t n = s->n;
    uint32_t T = nd_whistle_frames(s->w, n);
    float   *x   = psram((size_t)n * sizeof(float));
    float   *enc = psram((size_t)T * s->w->d * sizeof(float));
    bool     ok  = false;

    if (x == NULL || enc == NULL) {
        ESP_LOGE(TAG, "no external memory to work in");
        goto out;
    }
    for (uint32_t i = 0; i < n; i++) x[i] = s->pcm[i] / 32768.0f;

    int64_t t0 = esp_timer_get_time();
    if (nd_whistle_encode(s->w, x, n, enc) != 0) goto out;
    int64_t t1 = esp_timer_get_time();

    uint32_t ids[MAX_TOKENS];
    int nt = nd_whistle_decode(s->w, enc, T, -1, ids, MAX_TOKENS);
    int64_t t2 = esp_timer_get_time();
    if (nt < 0) goto out;

    nd_whistle_detok(s->w, ids, (uint32_t)nt, s->text, TEXT_MAX - 1);
    s->text[TEXT_MAX - 2] = '\0';
    s->text_len = (uint32_t)strlen(s->text);
    s->text[s->text_len++] = '\n';
    s->text_pos = 0;

    s->timing.audio_ms  = n / (RATE / 1000);
    s->timing.encode_ms = (uint32_t)((t1 - t0) / 1000);
    s->timing.decode_ms = (uint32_t)((t2 - t1) / 1000);
    ok = true;

out:
    heap_caps_free(x);
    heap_caps_free(enc);
    say_memory("after");
    return ok;
}

static void engine_task(void *arg)
{
    stt_t *s = (stt_t *)arg;
    for (;;) {
        if (s->state == ST_QUEUED) {
            s->state = ST_RUNNING;
            s->timing.load_ms = 0;
            bool ok = (s->loaded || load_model(s)) && recognise(s);
            s->state = ok ? ST_DONE : ST_FAILED;
        }
        rv9_task_delay_ms(20);
    }
}

/* ---- the caller's side ---- */

static rv9_io_err_t stt_init(rv9_dev_t *dev)
{
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < (32u << 20) / 2) {
        ESP_LOGI(TAG, "%s: not enough external memory for a model", dev->name);
        return RV9_IO_ERR_NOTFOUND;
    }
    stt_t *s = rv9_calloc(1, sizeof(*s));
    if (s == NULL) return RV9_IO_ERR_NOMEM;
    s->dev = dev;
    strncpy(s->model, DEFAULT_MODEL, sizeof(s->model) - 1);
    dev->drv_state = s;
    ESP_LOGI(TAG, "%s: Whistle, depth %d; the model is read on first use",
             dev->name, DEPTH);
    return RV9_IO_OK;
}

/* The first open pays for what a recogniser needs; nothing before that. */
static rv9_io_err_t stt_open(rv9_dev_t *dev, uint32_t mode)
{
    (void)mode;
    stt_t *s = (stt_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    if (s->pcm == NULL) {
        s->pcm = psram((size_t)CAP_SAMPLES * sizeof(int16_t));
        if (s->pcm == NULL) return RV9_IO_ERR_NOMEM;
    }
    if (!s->engine_started) {
        if (rv9_task_create_background(engine_task, "rv9-stt", ENGINE_STACK,
                                       s) != RV9_OK) {
            return RV9_IO_ERR_NOMEM;
        }
        s->engine_started = true;
    }
    if (s->state != ST_QUEUED && s->state != ST_RUNNING) {
        s->state = ST_IDLE;
        s->n = 0;
    }
    return RV9_IO_OK;
}

static rv9_io_err_t stt_write(rv9_dev_t *dev, const void *buf, size_t len,
                              size_t *done)
{
    stt_t *s = (stt_t *)dev->drv_state;
    if (done) *done = 0;
    if (s == NULL || s->pcm == NULL) return RV9_IO_ERR_IO;

    /* Audio after an answer nobody read starts the next utterance. */
    if (s->state == ST_DONE || s->state == ST_FAILED) {
        s->state = ST_IDLE;
        s->n = 0;
    }
    if (s->state != ST_IDLE) return RV9_IO_ERR_BUSY;

    uint32_t k = (uint32_t)(len / 2);
    if (k > CAP_SAMPLES - s->n) return RV9_IO_ERR_NOMEM;   /* 30 s is the most */
    memcpy(s->pcm + s->n, buf, (size_t)k * 2);
    s->n += k;
    if (done) *done = (size_t)k * 2;
    return RV9_IO_OK;
}

static rv9_io_err_t stt_read(rv9_dev_t *dev, void *buf, size_t len,
                             size_t *done)
{
    stt_t *s = (stt_t *)dev->drv_state;
    if (done) *done = 0;
    if (s == NULL || buf == NULL || len == 0) return RV9_IO_ERR_IO;

    switch (s->state) {
    case ST_QUEUED:
    case ST_RUNNING:
        return RV9_IO_ERR_WOULDBLOCK;       /* SCF sleeps and asks again */
    case ST_FAILED:
        s->state = ST_IDLE;
        s->n = 0;
        return RV9_IO_ERR_IO;
    case ST_DONE:
        break;
    default:
        return RV9_IO_ERR_INVAL;            /* nothing was asked */
    }

    uint32_t left = s->text_len - s->text_pos;
    uint32_t k = left < len ? left : (uint32_t)len;
    memcpy(buf, s->text + s->text_pos, k);
    s->text_pos += k;
    if (s->text_pos >= s->text_len) {
        s->state = ST_IDLE;
        s->n = 0;
    }
    if (done) *done = k;
    return RV9_IO_OK;
}

static rv9_io_err_t stt_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    stt_t *s = (stt_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;
    if (code != RV9_STT_GS_TIMING) return RV9_IO_ERR_UNSUPPORTED;
    if (arg == NULL) return RV9_IO_ERR_INVAL;
    memcpy(arg, &s->timing, sizeof(s->timing));
    return RV9_IO_OK;
}

static rv9_io_err_t stt_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    stt_t *s = (stt_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    if (code == RV9_STT_SS_DONE) {
        if (s->state != ST_IDLE) return RV9_IO_ERR_BUSY;
        if (s->n < RATE / 10) return RV9_IO_ERR_INVAL;   /* under 0.1 s */
        s->state = ST_QUEUED;
        return RV9_IO_OK;
    }
    if (code == RV9_STT_SS_MODEL) {
        const char *path = (const char *)arg;
        if (path == NULL || path[0] == '\0') return RV9_IO_ERR_INVAL;
        if (s->loaded) return RV9_IO_ERR_BUSY;   /* one model per boot */
        strncpy(s->model, path, sizeof(s->model) - 1);
        s->model[sizeof(s->model) - 1] = '\0';
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

#else   /* no PSRAM to hold a model, and no vector unit to run it */

static rv9_io_err_t stt_init(rv9_dev_t *dev)
{
    ESP_LOGI(TAG, "%s: speech recognition is the P4's", dev->name);
    return RV9_IO_ERR_NOTFOUND;
}

#define stt_open    NULL
#define stt_read    NULL
#define stt_write   NULL
#define stt_getstat NULL
#define stt_setstat NULL

#endif

static const rv9_driver_t stt = {
    .name       = "stt",
    .raw_stream = true,
    .init       = stt_init,
    .open       = stt_open,
    .read       = stt_read,
    .write      = stt_write,
    .getstat    = stt_getstat,
    .setstat    = stt_setstat,
};

rv9_io_err_t rv9_drv_stt_register(void)
{
    return rv9_io_register_driver(&stt);
}
