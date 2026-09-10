/*
 * ramdisk -- a block device made of memory.
 *
 * Exists so that RBF can be built and proven without depending on whether
 * the SD card driver works, or on whether there is a card in the slot. When
 * sdspi arrives it implements the same three entry points and RBF will not
 * know the difference -- which is the claim the layering makes, and this is
 * the cheapest way to test it.
 *
 * Descriptor options:
 *   opt[2]  sector count (default 128, giving 64 KB at 512 bytes)
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-ramdisk";

#define SECTOR_SIZE     512
#define OPT_SECTORS     2
#define DEFAULT_SECTORS 128

typedef struct {
    uint8_t  *data;
    uint32_t  sectors;
} ramdisk_t;

static rv9_io_err_t ramdisk_init(rv9_dev_t *dev)
{
    uint32_t sectors = dev->opt[OPT_SECTORS] ? dev->opt[OPT_SECTORS]
                                             : DEFAULT_SECTORS;

    ramdisk_t *r = rv9_calloc(1, sizeof(*r));
    if (r == NULL) return RV9_IO_ERR_NOMEM;

    r->data = rv9_calloc(sectors, SECTOR_SIZE);
    if (r->data == NULL) {
        rv9_free(r);
        ESP_LOGE(TAG, "cannot allocate %lu sectors",
                 (unsigned long)sectors);
        return RV9_IO_ERR_NOMEM;
    }

    r->sectors = sectors;
    dev->drv_state = r;

    ESP_LOGI(TAG, "%lu sectors of %d bytes (%lu KB)",
             (unsigned long)sectors, SECTOR_SIZE,
             (unsigned long)(sectors * SECTOR_SIZE / 1024));
    return RV9_IO_OK;
}

static rv9_io_err_t ramdisk_geometry(rv9_dev_t *dev, uint32_t *sector_size,
                                     uint32_t *sector_count)
{
    ramdisk_t *r = (ramdisk_t *)dev->drv_state;
    if (r == NULL) return RV9_IO_ERR_IO;

    if (sector_size)  *sector_size  = SECTOR_SIZE;
    if (sector_count) *sector_count = r->sectors;
    return RV9_IO_OK;
}

static rv9_io_err_t ramdisk_read(rv9_dev_t *dev, uint32_t lsn, void *buf,
                                 uint32_t count)
{
    ramdisk_t *r = (ramdisk_t *)dev->drv_state;
    if (r == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > r->sectors) return RV9_IO_ERR_INVAL;

    memcpy(buf, r->data + (size_t)lsn * SECTOR_SIZE,
           (size_t)count * SECTOR_SIZE);
    return RV9_IO_OK;
}

static rv9_io_err_t ramdisk_write(rv9_dev_t *dev, uint32_t lsn,
                                  const void *buf, uint32_t count)
{
    ramdisk_t *r = (ramdisk_t *)dev->drv_state;
    if (r == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > r->sectors) return RV9_IO_ERR_INVAL;

    memcpy(r->data + (size_t)lsn * SECTOR_SIZE, buf,
           (size_t)count * SECTOR_SIZE);
    return RV9_IO_OK;
}

static const rv9_driver_t ramdisk = {
    .name         = "ramdisk",
    .init         = ramdisk_init,
    .geometry     = ramdisk_geometry,
    .read_blocks  = ramdisk_read,
    .write_blocks = ramdisk_write,
};

rv9_io_err_t rv9_drv_ramdisk_register(void)
{
    return rv9_io_register_driver(&ramdisk);
}
