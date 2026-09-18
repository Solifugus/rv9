/*
 * pipemem -- somewhere to keep pipes.
 *
 * The whole driver, and deliberately the same shape as pubmem: a block of
 * memory allocated once at attach, and an answer to where it is.
 * Everything that makes a pipe a pipe -- names, the ring, who is reading,
 * when the last writer has gone -- happens above it in PFMPIPE.
 *
 * Allocated once and never grown, because a pipe that can fail to exist
 * partway through a command line is worse than a pipe that is refused at
 * the start. The cost is memory spent at boot whether anything pipes or
 * not, so the defaults are small: four pipes of 256 bytes is 1 KB, and a
 * shell pipeline carrying the output of `dir` never comes close.
 *
 * Descriptor options:
 *   opt[0]  bytes of buffer per pipe (default 256)
 *   opt[1]  number of pipes          (default 4)
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include "esp_log.h"

static const char *TAG = "rv9-pipemem";

#define OPT_PIPE_BYTES  0
#define OPT_PIPE_COUNT  1

#define DEFAULT_PIPE_BYTES 256
#define DEFAULT_PIPE_COUNT 4

/* A ceiling on what one descriptor may ask for. The store comes out of the
   same heap as everything else, and a typo in a descriptor should be
   refused at boot rather than starve the network stack at runtime. */
#define MAX_STORE_BYTES 8192

typedef struct {
    void     *base;
    uint32_t  size;
} pipemem_t;

static rv9_io_err_t pipemem_init(rv9_dev_t *dev)
{
    uint32_t bytes = dev->opt[OPT_PIPE_BYTES] ? dev->opt[OPT_PIPE_BYTES]
                                              : DEFAULT_PIPE_BYTES;
    uint32_t count = dev->opt[OPT_PIPE_COUNT] ? dev->opt[OPT_PIPE_COUNT]
                                              : DEFAULT_PIPE_COUNT;

    /* The file manager owns the layout; this adds what its per-pipe
       bookkeeping needs, exactly as pubmem does for PFM. */
    uint32_t size = count * (bytes + 64);

    if (size == 0 || size > MAX_STORE_BYTES) {
        ESP_LOGE(TAG, "%s: %lu pipes of %lu bytes is %lu, past the %d limit",
                 dev->name, (unsigned long)count, (unsigned long)bytes,
                 (unsigned long)size, MAX_STORE_BYTES);
        return RV9_IO_ERR_INVAL;
    }

    pipemem_t *m = rv9_calloc(1, sizeof(*m));
    if (m == NULL) return RV9_IO_ERR_NOMEM;

    m->base = rv9_calloc(1, size);
    if (m->base == NULL) {
        rv9_free(m);
        ESP_LOGE(TAG, "%s: cannot reserve %lu bytes", dev->name,
                 (unsigned long)size);
        return RV9_IO_ERR_NOMEM;
    }

    m->size = size;
    dev->drv_state = m;

    ESP_LOGI(TAG, "%s: %lu bytes for up to %lu pipes of %lu", dev->name,
             (unsigned long)size, (unsigned long)count, (unsigned long)bytes);
    return RV9_IO_OK;
}

static rv9_io_err_t pipemem_arena(rv9_dev_t *dev, void **base, uint32_t *size)
{
    pipemem_t *m = (pipemem_t *)dev->drv_state;
    if (m == NULL) return RV9_IO_ERR_IO;

    if (base) *base = m->base;
    if (size) *size = m->size;
    return RV9_IO_OK;
}

static const rv9_driver_t pipemem = {
    .name = "pipemem",
    /*
     * Unlike a published value, a pipe is emphatically *not* kept when the
     * last path to it closes. What is left in a pipe nobody is reading is
     * not a record of anything -- it is half a sentence, and the next
     * command to use that name would read it as the start of its own.
     */
    .retains = false,
    .init    = pipemem_init,
    .arena   = pipemem_arena,
};

rv9_io_err_t rv9_drv_pipemem_register(void)
{
    return rv9_io_register_driver(&pipemem);
}
