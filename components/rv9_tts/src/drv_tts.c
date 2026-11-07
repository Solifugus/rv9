/*
 * tts driver -- CMU Flite speaking, as /tts0.
 *
 * Text goes in by write, the end of it is said with a setstat, and the
 * speech comes out by read. See RV9_TTS_* in rv9/module.h for what a
 * program sees; this is how it is done.
 *
 * WHERE THE WORK HAPPENS
 *
 * In a host task of its own, below all of RV-9's, so that a
 * second of arithmetic per sentence stops nobody. The caller's read says
 * "not yet" until there are samples, which SCF turns into a sleep.
 *
 * Unlike a recogniser's, the answer is not one line at the end. Flite speaks a text
 * a sentence at a time and hands over each sentence's samples as the
 * vocoder makes them (its "streaming" callback), so they go into a ring
 * here and the reader takes them as they arrive: the first sound comes
 * after the first sentence's analysis, not after the whole text. When the
 * ring is full the engine waits for the reader -- the voice is made faster
 * than it plays, so a reader feeding a speaker keeps it waiting most of
 * the time, and the ring stays small.
 *
 * WHAT IT HOLDS
 *
 * While anybody is speaking, and for IDLE_MS after: the voice, about 3 MB
 * of external memory once loaded (design section 64), a 128 KB ring and
 * the engine task's 16 KB stack. Then the engine unloads the voice, frees
 * the ring and ends, and the next open starts it all again. Reading the
 * voice costs 1.6 s at the next `say`; holding it cost `listen` six
 * seconds of speech, because Flite's objects sit in the middle of external
 * memory and the largest block left was 25 MB of 32 (design section 70).
 * The grace is for a script saying several things in a row.
 *
 * Everything Flite allocates comes from external memory (CST_USER_MALLOC,
 * below): it makes thousands of small objects per sentence, and the
 * internal heap is everybody's.
 *
 * FLITE'S ERRORS
 *
 * Flite reports a failure by longjmp to cst_errjmp, or by exit() if none
 * is set -- which here would restart the board. So every call into it is
 * made with cst_errjmp set, and a failure is a failed text, not a reboot.
 * What it had allocated for that text is lost: Flite keeps no list.
 *
 * Handing over is by one word, `state`: the caller moves it IDLE ->
 * QUEUED, the engine QUEUED -> RUNNING -> DONE or FAILED, the caller back
 * to IDLE once it has read the end. The ring has one writer, the engine,
 * and one reader, the caller, each owning its own index.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/tts.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-tts";

#if CONFIG_IDF_TARGET_ESP32P4

#include <setjmp.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "flite.h"
#include "rv9_flite.h"

#define TEXT_MAX      1024
#define RING_SAMPLES  (64 * 1024)       /* 4 s at 16 kHz */
#define CHUNK_SAMPLES 1024              /* what Flite hands over at a time */
#define READ_CHUNK    (64 * 1024)
#define ENGINE_STACK  (16 * 1024)       /* Flite measured 1.5 KB at its peak */

#define DEFAULT_VOICE "/sd0/slt.flitevox"
#define IDLE_MS       3000              /* after the last close, then give it back */

enum { ST_IDLE, ST_QUEUED, ST_RUNNING, ST_DONE, ST_FAILED };

typedef struct {
    rv9_dev_t       *dev;
    volatile int     state;
    volatile bool    stop;          /* the reader went away: stop speaking */

    char             text[TEXT_MAX + 1];
    uint32_t         text_len;

    int16_t         *ring;          /* PSRAM, RING_SAMPLES */
    volatile uint32_t put, get;     /* free-running; the engine owns put */

    char             voice[48];
    bool             loaded;

    /* Under `lock`: whether the engine task and the ring exist, whether a
       path is open, and whether the engine is giving everything back. */
    rv9_lock_t       lock;
    bool             engine_started, in_use, ending;
    int64_t          closed_at;
    cst_voice       *v;
    uint32_t         rate;

    int64_t          asked_at;
    bool             first;
    uint32_t         samples;
    rv9_tts_timing_t timing;
} tts_t;

/* ---- Flite's memory: external, all of it ---- */

void *cst_safe_alloc(int size)
{
    if (size < 0) {
        cst_errmsg("alloc: asked for negative size %d\n", size);
        cst_error();
    }
    void *p = heap_caps_calloc(1, size ? (size_t)size : 1, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        cst_errmsg("alloc: can't alloc %d bytes\n", size);
        cst_error();
    }
    return p;
}

void *cst_safe_calloc(int size) { return cst_safe_alloc(size); }

void *cst_safe_realloc(void *p, int size)
{
    if (p == NULL) return cst_safe_alloc(size);
    void *np = heap_caps_realloc(p, size ? (size_t)size : 1, MALLOC_CAP_SPIRAM);
    if (np == NULL) {
        cst_errmsg("CST_REALLOC failed for %d bytes\n", size);
        cst_error();
    }
    return np;
}

void cst_free(void *p) { heap_caps_free(p); }

/* ---- the engine's side ---- */

static void say_memory(const char *when)
{
    ESP_LOGI(TAG, "%s: internal %u free; external %u free", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* Flite's streaming callback: this sentence's samples, as they are made. */
static int chunk(const cst_wave *w, int start, int size, int last,
                 cst_audio_streaming_info *asi)
{
    (void)last;
    tts_t *s = (tts_t *)asi->userdata;

    if (!s->first) {
        s->first = true;
        s->timing.first_ms =
            (uint32_t)((esp_timer_get_time() - s->asked_at) / 1000);
    }
    for (int i = 0; i < size; ) {
        if (s->stop) return CST_AUDIO_STREAM_STOP;
        uint32_t room = RING_SAMPLES - (s->put - __atomic_load_n(&s->get,
                                                         __ATOMIC_ACQUIRE));
        if (room == 0) {
            rv9_task_delay_ms(10);          /* the reader is behind: wait */
            continue;
        }
        uint32_t k = (uint32_t)(size - i) < room ? (uint32_t)(size - i) : room;
        for (uint32_t j = 0; j < k; j++) {
            s->ring[(s->put + j) % RING_SAMPLES] = w->samples[start + i + j];
        }
        __atomic_store_n(&s->put, s->put + k, __ATOMIC_RELEASE);
        i += (int)k;
        s->samples += k;
    }
    return CST_AUDIO_STREAM_CONT;
}

/* As the system, from whatever volume the path names. */
static bool load_voice(tts_t *s)
{
    int64_t t0 = esp_timer_get_time();
    say_memory("loading");

    int p = rv9_io_open(s->voice, RV9_MODE_READ);
    if (p < 0) {
        ESP_LOGE(TAG, "%s: cannot open the voice", s->voice);
        return false;
    }
    uint64_t size = 0;
    if (rv9_io_getstat(p, RV9_GS_SIZE, &size) != RV9_IO_OK || size == 0) {
        ESP_LOGE(TAG, "%s: cannot tell how big it is", s->voice);
        rv9_io_close(p);
        return false;
    }
    uint8_t *blob = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (blob == NULL) {
        ESP_LOGE(TAG, "no %lu bytes of external memory for the voice",
                 (unsigned long)size);
        rv9_io_close(p);
        return false;
    }
    uint64_t have = 0;
    while (have < size) {
        size_t want = (size - have) < READ_CHUNK ? (size_t)(size - have)
                                                 : READ_CHUNK;
        size_t got = 0;
        if (rv9_io_read(p, blob + have, want, &got) != RV9_IO_OK || got == 0) {
            break;
        }
        have += got;
    }
    rv9_io_close(p);
    if (have != size) {
        ESP_LOGE(TAG, "%s: read %lu of %lu bytes", s->voice,
                 (unsigned long)have, (unsigned long)size);
        heap_caps_free(blob);
        return false;
    }

    /* Flite copies what it reads, so the file's bytes go once it has. */
    jmp_buf jb;
    cst_errjmp = &jb;
    if (setjmp(jb) == 0) {
        /* Once a boot: the languages are a list, and they would be on it
           twice the second time a voice was read. */
        static bool flite_up;
        if (!flite_up) {
            flite_init();
            flite_add_lang("eng", usenglish_init, cmulex_init);
            flite_add_lang("usenglish", usenglish_init, cmulex_init);
            flite_up = true;
        }
        rv9_flite_memfile(s->voice, blob, (size_t)size);
        s->v = flite_voice_load(s->voice);
    } else {
        s->v = NULL;
    }
    cst_errjmp = NULL;
    rv9_flite_memfile(NULL, NULL, 0);
    heap_caps_free(blob);

    if (s->v == NULL) {
        ESP_LOGE(TAG, "%s is not a voice Flite reads", s->voice);
        return false;
    }

    cst_audio_streaming_info *asi = new_audio_streaming_info();
    asi->min_buffsize = CHUNK_SAMPLES;
    asi->asc = chunk;
    asi->userdata = s;
    feat_set(s->v->features, "streaming_info", audio_streaming_info_val(asi));

    s->rate = (uint32_t)get_param_int(s->v->features, "sample_rate", 16000);
    s->timing.load_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%s: %s, %lu bytes in %lu ms, %lu Hz", s->voice,
             get_param_string(s->v->features, "name", "a voice"),
             (unsigned long)size, (unsigned long)s->timing.load_ms,
             (unsigned long)s->rate);
    say_memory("loaded");
    s->loaded = true;
    return true;
}

static bool speak(tts_t *s)
{
    s->first = false;
    s->samples = 0;

    int64_t t0 = esp_timer_get_time();
    bool ok;
    jmp_buf jb;
    cst_errjmp = &jb;
    if (setjmp(jb) == 0) {
        /* A token stream, not flite_text_to_speech: that makes the whole
           text one utterance, so nothing is heard until all of it has
           been worked out. This way each sentence is spoken as soon as
           it is ready, with Flite's pause after it. */
        cst_features *f = s->v->features;
        cst_tokenstream *ts = ts_open_string(s->text,
            get_param_string(f, "text_whitespace", NULL),
            get_param_string(f, "text_singlecharsymbols", NULL),
            get_param_string(f, "text_prepunctuation", NULL),
            get_param_string(f, "text_postpunctuation", NULL));
        flite_ts_to_speech(ts, s->v, "none");        /* and closes ts */
        ok = true;
    } else {
        ESP_LOGE(TAG, "Flite failed partway; what it held for this text is lost");
        ok = false;
    }
    cst_errjmp = NULL;

    s->timing.synth_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s->timing.audio_ms = s->samples / (s->rate / 1000);
    if (!s->first) s->timing.first_ms = 0;
    return ok || s->stop;
}

static void unload_voice(tts_t *s)
{
    if (!s->loaded) return;
    jmp_buf jb;
    cst_errjmp = &jb;
    if (setjmp(jb) == 0) delete_voice(s->v);  /* its database and features */
    cst_errjmp = NULL;
    s->v = NULL;
    s->loaded = false;
    say_memory("voice given back");
}

/* Nobody has had it open for IDLE_MS, and nothing is waiting to be said. */
static bool time_to_go(tts_t *s)
{
    bool go = false;
    rv9_lock_acquire(s->lock);
    if (!s->in_use && s->state != ST_QUEUED && s->state != ST_RUNNING &&
        esp_timer_get_time() - s->closed_at >= (int64_t)IDLE_MS * 1000) {
        s->ending = true;               /* an open now waits for the end */
        go = true;
    }
    rv9_lock_release(s->lock);
    return go;
}

static void engine_task(void *arg)
{
    tts_t *s = (tts_t *)arg;
    for (;;) {
        if (s->state == ST_QUEUED) {
            s->state = ST_RUNNING;
            s->timing.load_ms = 0;
            bool ok = (s->loaded || load_voice(s)) && speak(s);
            s->state = ok ? ST_DONE : ST_FAILED;
        } else if (time_to_go(s)) {
            break;
        }
        rv9_task_delay_ms(20);
    }

    /* Flite is this task's alone, so the voice goes here; the ring is the
       reader's too, and nobody reads with no path open. */
    unload_voice(s);
    rv9_lock_acquire(s->lock);
    heap_caps_free(s->ring);
    s->ring = NULL;
    s->put = s->get = 0;
    s->state = ST_IDLE;
    s->engine_started = false;
    s->ending = false;
    rv9_lock_release(s->lock);
    rv9_task_delete(NULL);
}

/* ---- the caller's side ---- */

static void reset(tts_t *s)
{
    s->text_len = 0;
    s->put = s->get = 0;
    s->stop = false;
    s->state = ST_IDLE;
}

static rv9_io_err_t tts_init(rv9_dev_t *dev)
{
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < (8u << 20)) {
        ESP_LOGI(TAG, "%s: not enough external memory for a voice", dev->name);
        return RV9_IO_ERR_NOTFOUND;
    }
    tts_t *s = rv9_calloc(1, sizeof(*s));
    if (s == NULL) return RV9_IO_ERR_NOMEM;
    if (rv9_lock_create(&s->lock) != RV9_OK) {
        rv9_free(s);
        return RV9_IO_ERR_NOMEM;
    }
    s->dev = dev;
    s->rate = 16000;
    strncpy(s->voice, DEFAULT_VOICE, sizeof(s->voice) - 1);
    dev->drv_state = s;
    ESP_LOGI(TAG, "%s: Flite; the voice is read on first use", dev->name);
    return RV9_IO_OK;
}

/* An open pays for what a voice needs, if the engine has given it back. */
static rv9_io_err_t tts_open(rv9_dev_t *dev, uint32_t mode)
{
    (void)mode;
    tts_t *s = (tts_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    rv9_lock_acquire(s->lock);
    while (s->ending) {                 /* tens of milliseconds, not held */
        rv9_lock_release(s->lock);
        rv9_task_delay_ms(10);
        rv9_lock_acquire(s->lock);
    }
    rv9_io_err_t err = RV9_IO_OK;
    if (!s->engine_started) {
        s->ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM);
        if (s->ring == NULL) {
            err = RV9_IO_ERR_NOMEM;
        } else if (rv9_task_create_background(engine_task, "rv9-tts",
                                              ENGINE_STACK, s) != RV9_OK) {
            heap_caps_free(s->ring);
            s->ring = NULL;
            err = RV9_IO_ERR_NOMEM;
        } else {
            s->engine_started = true;
        }
    }
    if (err == RV9_IO_OK) {
        s->in_use = true;
        if (s->state != ST_QUEUED && s->state != ST_RUNNING) reset(s);
    }
    rv9_lock_release(s->lock);
    return err;
}

/* The last path has gone: whatever is still being said, nobody hears it. */
static rv9_io_err_t tts_close(rv9_dev_t *dev)
{
    tts_t *s = (tts_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_OK;
    rv9_lock_acquire(s->lock);
    if (s->state == ST_QUEUED) {
        reset(s);
    } else if (s->state == ST_RUNNING) {
        s->stop = true;             /* the engine sees it at the next chunk */
    }
    s->in_use = false;
    s->closed_at = esp_timer_get_time();
    rv9_lock_release(s->lock);
    return RV9_IO_OK;
}

static rv9_io_err_t tts_write(rv9_dev_t *dev, const void *buf, size_t len,
                              size_t *done)
{
    tts_t *s = (tts_t *)dev->drv_state;
    if (done) *done = 0;
    if (s == NULL || s->ring == NULL) return RV9_IO_ERR_IO;

    /* Text after a voice nobody finished reading starts the next one. */
    if (s->state == ST_DONE || s->state == ST_FAILED) reset(s);
    if (s->state != ST_IDLE) return RV9_IO_ERR_BUSY;

    if (len > TEXT_MAX - s->text_len) return RV9_IO_ERR_NOMEM;   /* 1 KB */
    memcpy(s->text + s->text_len, buf, len);
    s->text_len += (uint32_t)len;
    s->text[s->text_len] = '\0';
    if (done) *done = len;
    return RV9_IO_OK;
}

static rv9_io_err_t tts_read(rv9_dev_t *dev, void *buf, size_t len,
                             size_t *done)
{
    tts_t *s = (tts_t *)dev->drv_state;
    if (done) *done = 0;
    if (s == NULL || buf == NULL || len < 2) return RV9_IO_ERR_IO;

    uint32_t have = __atomic_load_n(&s->put, __ATOMIC_ACQUIRE) - s->get;
    if (have > 0) {
        uint32_t k = (uint32_t)(len / 2) < have ? (uint32_t)(len / 2) : have;
        int16_t *out = (int16_t *)buf;
        for (uint32_t j = 0; j < k; j++) {
            out[j] = s->ring[(s->get + j) % RING_SAMPLES];
        }
        __atomic_store_n(&s->get, s->get + k, __ATOMIC_RELEASE);
        if (done) *done = (size_t)k * 2;
        return RV9_IO_OK;
    }

    switch (s->state) {
    case ST_QUEUED:
    case ST_RUNNING:
        return RV9_IO_ERR_WOULDBLOCK;       /* SCF sleeps and asks again */
    case ST_FAILED:
        reset(s);
        return RV9_IO_ERR_IO;
    case ST_DONE:
        reset(s);
        return RV9_IO_ERR_INVAL;            /* all of it has been read */
    default:
        return RV9_IO_ERR_INVAL;            /* nothing was asked */
    }
}

static rv9_io_err_t tts_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    tts_t *s = (tts_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;
    if (arg == NULL) return RV9_IO_ERR_INVAL;
    if (code == RV9_TTS_GS_TIMING) {
        memcpy(arg, &s->timing, sizeof(s->timing));
        return RV9_IO_OK;
    }
    if (code == RV9_TTS_GS_FORMAT) {
        rv9_audio_format_t f = {
            .rate = s->rate, .channels = 1, .bits = 16,
            .in_channels = 0, .out_channels = 1,
        };
        memcpy(arg, &f, sizeof(f));
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t tts_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    tts_t *s = (tts_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    if (code == RV9_TTS_SS_DONE) {
        if (s->state != ST_IDLE) return RV9_IO_ERR_BUSY;
        if (s->text_len == 0) return RV9_IO_ERR_INVAL;
        s->asked_at = esp_timer_get_time();
        s->state = ST_QUEUED;
        return RV9_IO_OK;
    }
    if (code == RV9_TTS_SS_VOICE) {
        const char *path = (const char *)arg;
        if (path == NULL || path[0] == '\0') return RV9_IO_ERR_INVAL;
        if (s->loaded) return RV9_IO_ERR_BUSY;   /* until it is given back */
        strncpy(s->voice, path, sizeof(s->voice) - 1);
        s->voice[sizeof(s->voice) - 1] = '\0';
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

#else   /* no PSRAM to hold a voice */

static rv9_io_err_t tts_init(rv9_dev_t *dev)
{
    ESP_LOGI(TAG, "%s: speech is the P4's", dev->name);
    return RV9_IO_ERR_NOTFOUND;
}

#define tts_open    NULL
#define tts_close   NULL
#define tts_read    NULL
#define tts_write   NULL
#define tts_getstat NULL
#define tts_setstat NULL

#endif

static const rv9_driver_t tts = {
    .name       = "tts",
    .raw_stream = true,
    .init       = tts_init,
    .open       = tts_open,
    .close      = tts_close,
    .read       = tts_read,
    .write      = tts_write,
    .getstat    = tts_getstat,
    .setstat    = tts_setstat,
};

rv9_io_err_t rv9_drv_tts_register(void)
{
    return rv9_io_register_driver(&tts);
}
