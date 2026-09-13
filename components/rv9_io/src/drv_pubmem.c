/*
 * pubmem -- somewhere to keep published values.
 *
 * The whole driver. It allocates a block of memory once, at attach, and
 * says where it is; everything that makes a publication a publication --
 * names, sequences, coherence, who may write -- happens above it in PFM.
 *
 * That division is not ceremony. Cells must be preallocated (R9 §18) and
 * must outlive the processes using them, so a store that is allocated once
 * and never grows is exactly right; and when a cell needs to live
 * somewhere else -- a region shared with a PMP-isolated process, memory
 * that survives a restart -- it is this file that is replaced and nothing
 * above it.
 *
 * Descriptor options:
 *   opt[0]  bytes of value per cell (default 64)
 *   opt[1]  number of cells         (default 16)
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include "esp_log.h"

static const char *TAG = "rv9-pubmem";

#define OPT_CELL_BYTES  0
#define OPT_CELL_COUNT  1

#define DEFAULT_CELL_BYTES 64
#define DEFAULT_CELL_COUNT 16

/* A ceiling on what one descriptor may ask for. The store is taken out of
   the same heap everything else runs in, and a typo in a descriptor should
   be refused at boot rather than starve the network stack at runtime. */
#define MAX_STORE_BYTES 8192

typedef struct {
    void     *base;
    uint32_t  size;
} pubmem_t;

static rv9_io_err_t pubmem_init(rv9_dev_t *dev)
{
    uint32_t bytes = dev->opt[OPT_CELL_BYTES] ? dev->opt[OPT_CELL_BYTES]
                                              : DEFAULT_CELL_BYTES;
    uint32_t count = dev->opt[OPT_CELL_COUNT] ? dev->opt[OPT_CELL_COUNT]
                                              : DEFAULT_CELL_COUNT;

    /*
     * The file manager decides the stride, not this file, so the size
     * asked for here is generous by exactly the per-cell bookkeeping PFM
     * adds. Passing the two numbers through the descriptor options and
     * letting PFM read them again is the alternative, and it would put the
     * layout in two places.
     */
    uint32_t size = count * (bytes + 64);

    if (size == 0 || size > MAX_STORE_BYTES) {
        ESP_LOGE(TAG, "%s: %lu cells of %lu bytes is %lu, past the %d limit",
                 dev->name, (unsigned long)count, (unsigned long)bytes,
                 (unsigned long)size, MAX_STORE_BYTES);
        return RV9_IO_ERR_INVAL;
    }

    pubmem_t *m = rv9_calloc(1, sizeof(*m));
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

    ESP_LOGI(TAG, "%s: %lu bytes for up to %lu cells of %lu", dev->name,
             (unsigned long)size, (unsigned long)count, (unsigned long)bytes);
    return RV9_IO_OK;
}

static rv9_io_err_t pubmem_arena(rv9_dev_t *dev, void **base, uint32_t *size)
{
    pubmem_t *m = (pubmem_t *)dev->drv_state;
    if (m == NULL) return RV9_IO_ERR_IO;

    if (base) *base = m->base;
    if (size) *size = m->size;
    return RV9_IO_OK;
}

static const rv9_driver_t pubmem = {
    .name    = "pubmem",
    /*
     * A published value is emphatically kept when the last path to it
     * closes. A control loop that stops leaves behind the last thing it
     * saw and when it saw it, which is exactly what whatever comes to
     * investigate needs to read.
     */
    .retains = true,
    .init    = pubmem_init,
    .arena   = pubmem_arena,
};

rv9_io_err_t rv9_drv_pubmem_register(void)
{
    return rv9_io_register_driver(&pubmem);
}
