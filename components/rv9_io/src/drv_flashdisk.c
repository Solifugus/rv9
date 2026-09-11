/*
 * flashdisk -- a block device backed by a flash partition.
 *
 * The point is persistence. A RAM disk loses every file and every loaded
 * program at reset, which is fine for proving a file manager works and
 * useless for anything real. This board has spare flash; that is a volume.
 *
 * The awkward part is that flash erases in 4 KB blocks and RBF writes in
 * 512-byte sectors. A partial write therefore means read the whole erase
 * block, patch the sectors inside it, erase, and write it back. That is
 * eight times the write traffic in the worst case, and the driver batches
 * by erase block so a sequential run of sectors costs one cycle rather
 * than eight.
 *
 * Flash wears out -- on the order of 100k erase cycles per block. This is
 * fine for configuration, programs and logs written occasionally. It is
 * the wrong device for something rewritten every second, and when that
 * matters the answer is an SD card behind the same interface, which RBF
 * will not notice.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "rv9-flashdisk";

#define SECTOR_SIZE 512
#define ERASE_SIZE  4096
#define SECTORS_PER_ERASE (ERASE_SIZE / SECTOR_SIZE)

/* Descriptor options: opt[2] is the partition label, unused -- the label
   comes from opt via the descriptor's driver field being fixed, so the
   partition is named here. */
#define PARTITION_LABEL "vol0"

typedef struct {
    const esp_partition_t *part;
    uint32_t               sectors;
    uint8_t                scratch[ERASE_SIZE];
    rv9_mutex_t            lock;
} flashdisk_t;

static rv9_io_err_t flashdisk_init(rv9_dev_t *dev)
{
    flashdisk_t *f = rv9_calloc(1, sizeof(*f));
    if (f == NULL) return RV9_IO_ERR_NOMEM;

    f->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_ANY,
                                       PARTITION_LABEL);
    if (f->part == NULL) {
        ESP_LOGE(TAG, "no '%s' partition", PARTITION_LABEL);
        rv9_free(f);
        return RV9_IO_ERR_NOTFOUND;
    }

    if (rv9_mutex_create(&f->lock) != RV9_OK) {
        rv9_free(f);
        return RV9_IO_ERR_NOMEM;
    }

    f->sectors = f->part->size / SECTOR_SIZE;
    dev->drv_state = f;

    ESP_LOGI(TAG, "%lu sectors of %d bytes (%lu KB) on '%s'",
             (unsigned long)f->sectors, SECTOR_SIZE,
             (unsigned long)(f->part->size / 1024), PARTITION_LABEL);
    return RV9_IO_OK;
}

static rv9_io_err_t flashdisk_geometry(rv9_dev_t *dev, uint32_t *sector_size,
                                       uint32_t *sector_count)
{
    flashdisk_t *f = (flashdisk_t *)dev->drv_state;
    if (f == NULL) return RV9_IO_ERR_IO;

    if (sector_size)  *sector_size  = SECTOR_SIZE;
    if (sector_count) *sector_count = f->sectors;
    return RV9_IO_OK;
}

static rv9_io_err_t flashdisk_read(rv9_dev_t *dev, uint32_t lsn, void *buf,
                                   uint32_t count)
{
    flashdisk_t *f = (flashdisk_t *)dev->drv_state;
    if (f == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > f->sectors) return RV9_IO_ERR_INVAL;

    /* Reads are direct: flash is memory that happens to be slow. */
    esp_err_t err = esp_partition_read(f->part, (size_t)lsn * SECTOR_SIZE,
                                       buf, (size_t)count * SECTOR_SIZE);
    return (err == ESP_OK) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

static rv9_io_err_t flashdisk_write(rv9_dev_t *dev, uint32_t lsn,
                                    const void *buf, uint32_t count)
{
    flashdisk_t *f = (flashdisk_t *)dev->drv_state;
    if (f == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > f->sectors) return RV9_IO_ERR_INVAL;

    const uint8_t *src = (const uint8_t *)buf;
    rv9_io_err_t result = RV9_IO_OK;

    rv9_mutex_lock(f->lock, RV9_WAIT_FOREVER);

    uint32_t done = 0;
    while (done < count) {
        uint32_t sector = lsn + done;
        uint32_t block  = sector / SECTORS_PER_ERASE;
        uint32_t within = sector % SECTORS_PER_ERASE;

        /* How many of the remaining sectors fall inside this erase block?
           Batching them means one erase cycle instead of one per sector. */
        uint32_t here = SECTORS_PER_ERASE - within;
        if (here > count - done) here = count - done;

        size_t block_off = (size_t)block * ERASE_SIZE;

        if (esp_partition_read(f->part, block_off, f->scratch,
                               ERASE_SIZE) != ESP_OK) {
            result = RV9_IO_ERR_IO;
            break;
        }

        memcpy(f->scratch + (size_t)within * SECTOR_SIZE,
               src + (size_t)done * SECTOR_SIZE,
               (size_t)here * SECTOR_SIZE);

        if (esp_partition_erase_range(f->part, block_off, ERASE_SIZE) != ESP_OK ||
            esp_partition_write(f->part, block_off, f->scratch,
                                ERASE_SIZE) != ESP_OK) {
            result = RV9_IO_ERR_IO;
            break;
        }

        done += here;
    }

    rv9_mutex_unlock(f->lock);
    return result;
}

static const rv9_driver_t flashdisk = {
    .name         = "flashdisk",
    .init         = flashdisk_init,
    .geometry     = flashdisk_geometry,
    .read_blocks  = flashdisk_read,
    .write_blocks = flashdisk_write,
};

rv9_io_err_t rv9_drv_flashdisk_register(void)
{
    return rv9_io_register_driver(&flashdisk);
}
