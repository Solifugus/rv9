/*
 * sdspi -- the microSD slot, as a block device.
 *
 * RBF over this is the same file manager that runs on the RAM disk and on
 * the flash partition; only the driver underneath differs, which is the
 * whole point of the shape. `/sd0` is therefore gigabytes of the same
 * thing `/f0` gives in megabytes, without wearing out the flash the
 * firmware lives in.
 *
 * The card shares the display's SPI bus: one clock, one data-in, and a
 * chip select each (the card's on GPIO4, the display's on GPIO23). The
 * bus is brought up by whichever attaches first -- see
 * rv9_panel_bus_claim() -- and the SPI driver serialises transactions
 * between the two devices, so a redraw and a sector write cannot overlap.
 *
 * There is no card-detect line on this board, so a card is found by
 * trying to initialise one. No card means the device does not attach, and
 * `/sd0` is simply not there -- which is what a program asking for it
 * should be told, rather than finding a device that fails every read.
 *
 * Sectors move through a bounce buffer in DMA-capable memory because the
 * caller's may be anywhere; RBF reads and writes a sector at a time, so
 * this costs a copy and no more.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include "panel.h"

#include <string.h>

#include "esp_log.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "rv9-sdspi";

#define PIN_SD_CS    4
#define SECTOR_SIZE  512
#define CARD_KHZ     20000      /* SPI mode; the bus itself runs faster */

typedef struct {
    sdmmc_card_t      *card;
    sdspi_dev_handle_t slot;
    uint8_t           *bounce;   /* one sector, DMA-capable */
    rv9_lock_t         lock;
} sdspi_t;

static void sdspi_free(sdspi_t *s)
{
    if (s == NULL) return;
    if (s->lock) rv9_lock_destroy(s->lock);
    rv9_free(s->bounce);
    rv9_free(s->card);
    rv9_free(s);
}

static rv9_io_err_t sdspi_init(rv9_dev_t *dev)
{
    rv9_io_err_t err = rv9_panel_bus_claim();
    if (err != RV9_IO_OK) return err;

    sdspi_t *s = rv9_calloc(1, sizeof(*s));
    if (s == NULL) return RV9_IO_ERR_NOMEM;

    s->card   = rv9_calloc(1, sizeof(sdmmc_card_t));
    s->bounce = rv9_alloc_dma(SECTOR_SIZE);
    if (s->card == NULL || s->bounce == NULL ||
        rv9_lock_create(&s->lock) != RV9_OK) {
        sdspi_free(s);
        return RV9_IO_ERR_NOMEM;
    }

    if (sdspi_host_init() != ESP_OK) {
        ESP_LOGE(TAG, "sdspi host would not start");
        sdspi_free(s);
        return RV9_IO_ERR_IO;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = RV9_SPI_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;

    if (sdspi_host_init_device(&slot_cfg, &s->slot) != ESP_OK) {
        ESP_LOGE(TAG, "the card slot would not attach to the bus");
        sdspi_free(s);
        return RV9_IO_ERR_IO;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = s->slot;
    host.max_freq_khz = CARD_KHZ;

    if (sdmmc_card_init(&host, s->card) != ESP_OK) {
        /* Ordinary: the slot is empty. Said once, at info, because a board
           with no card in it is a normal board. */
        ESP_LOGI(TAG, "%s: no card in the slot", dev->name);
        sdspi_host_remove_device(s->slot);
        sdspi_free(s);
        return RV9_IO_ERR_NOTFOUND;
    }

    if (s->card->csd.sector_size != SECTOR_SIZE) {
        ESP_LOGE(TAG, "%s: %d-byte sectors, RV-9 wants %d",
                 dev->name, s->card->csd.sector_size, SECTOR_SIZE);
        sdspi_host_remove_device(s->slot);
        sdspi_free(s);
        return RV9_IO_ERR_UNSUPPORTED;
    }

    dev->drv_state = s;

    ESP_LOGI(TAG, "%s: card of %lu MB, %d-byte sectors",
             dev->name,
             (unsigned long)(((uint64_t)s->card->csd.capacity * SECTOR_SIZE) >> 20),
             s->card->csd.sector_size);
    return RV9_IO_OK;
}

static rv9_io_err_t sdspi_geometry(rv9_dev_t *dev, uint32_t *sector_size,
                                   uint32_t *sector_count)
{
    sdspi_t *s = (sdspi_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    if (sector_size)  *sector_size  = SECTOR_SIZE;
    if (sector_count) *sector_count = (uint32_t)s->card->csd.capacity;
    return RV9_IO_OK;
}

static rv9_io_err_t sdspi_read(rv9_dev_t *dev, uint32_t lsn, void *buf,
                               uint32_t count)
{
    sdspi_t *s = (sdspi_t *)dev->drv_state;
    if (s == NULL || buf == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > (uint32_t)s->card->csd.capacity) return RV9_IO_ERR_INVAL;

    uint8_t *dst = (uint8_t *)buf;
    rv9_io_err_t err = RV9_IO_OK;

    rv9_lock_acquire(s->lock);
    for (uint32_t i = 0; i < count; i++) {
        if (sdmmc_read_sectors(s->card, s->bounce, lsn + i, 1) != ESP_OK) {
            ESP_LOGW(TAG, "%s: read of sector %lu failed",
                     dev->name, (unsigned long)(lsn + i));
            err = RV9_IO_ERR_IO;
            break;
        }
        memcpy(dst + (size_t)i * SECTOR_SIZE, s->bounce, SECTOR_SIZE);
    }
    rv9_lock_release(s->lock);
    return err;
}

static rv9_io_err_t sdspi_write(rv9_dev_t *dev, uint32_t lsn, const void *buf,
                                uint32_t count)
{
    sdspi_t *s = (sdspi_t *)dev->drv_state;
    if (s == NULL || buf == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > (uint32_t)s->card->csd.capacity) return RV9_IO_ERR_INVAL;

    const uint8_t *src = (const uint8_t *)buf;
    rv9_io_err_t err = RV9_IO_OK;

    rv9_lock_acquire(s->lock);
    for (uint32_t i = 0; i < count; i++) {
        memcpy(s->bounce, src + (size_t)i * SECTOR_SIZE, SECTOR_SIZE);
        if (sdmmc_write_sectors(s->card, s->bounce, lsn + i, 1) != ESP_OK) {
            ESP_LOGW(TAG, "%s: write of sector %lu failed",
                     dev->name, (unsigned long)(lsn + i));
            err = RV9_IO_ERR_IO;
            break;
        }
    }
    rv9_lock_release(s->lock);
    return err;
}

static const rv9_driver_t sdspi = {
    .name         = "sdspi",
    .init         = sdspi_init,
    .geometry     = sdspi_geometry,
    .read_blocks  = sdspi_read,
    .write_blocks = sdspi_write,
};

rv9_io_err_t rv9_drv_sdspi_register(void)
{
    return rv9_io_register_driver(&sdspi);
}
