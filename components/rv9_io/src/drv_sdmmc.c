/*
 * The microSD slot as a block device, over SDMMC.
 *
 * Same driver name and same three entry points as drv_sdspi.c, which does
 * the same job over SPI on the C5 -- see that file's header for why RBF
 * over a card is the same file manager that runs on the RAM disk and the
 * flash partition. Only the transport differs, and the descriptor does not
 * have to know which: both register as `sdcard`, because the board decides
 * the transport and the module store is built once for both boards.
 *
 * WHAT IS DIFFERENT FROM SPI
 *
 * Four data lines instead of one, at 40 MHz instead of 20, and no chip
 * select and no shared bus -- the display here is on the DSI link, so
 * nothing has to be serialised against a redraw.
 *
 * And the slot has to be powered. The card's supply comes from one of the
 * chip's own regulators on this board, and without acquiring it the slot
 * is simply dead: no card found, no error that says why. LDO_VO4 is the
 * card's; LDO_VO3 is the display's D-PHY (see panel_dsi.c), and mixing
 * the two up costs an afternoon.
 *
 * The pin numbers and the regulator channel are from the board support
 * package the factory firmware named -- read as a datasheet, the same way
 * panel_dsi.c got the display's.
 *
 * There is no card-detect line, so a card is found by trying to initialise
 * one. No card means the device does not attach and `/sd0` is not there,
 * which is what a program asking for it should be told rather than finding
 * a device that fails every read.
 *
 * The legacy sdmmc_host API, deliberately: it is what drv_sdspi.c's
 * sdspi_host is the sibling of, so the two files stay readable side by
 * side. IDF v6 ships a newer sd_host_sdmmc.h and nothing here needs it
 * yet.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "rv9-sdcard";

#define PIN_CLK      43
#define PIN_CMD      44
#define PIN_D0       39
#define PIN_D1       40
#define PIN_D2       41
#define PIN_D3       42

#define SD_LDO_CHAN   4          /* LDO_VO4 feeds the slot on this board */
#define SD_WIDTH      4
#define SECTOR_SIZE 512

typedef struct {
    sdmmc_card_t *card;
    uint8_t      *bounce;   /* one sector, DMA-capable */
    rv9_lock_t    lock;
} sdcard_t;

/*
 * THE REGULATOR IS ACQUIRED ONCE AND NEVER GIVEN BACK, and that is not
 * laziness.
 *
 * An on-chip LDO channel is a rail, and a rail on a board this size feeds
 * more than the thing that asked for it. The first version of this file
 * released the channel on the no-card path -- acquire, fail to find a
 * card, tidy up -- and tidying up a shared supply is how you turn off
 * somebody else's hardware. Powering it down is not the inverse of
 * powering it up when you are not the only consumer.
 *
 * So it is static, taken at most once, and left on: the state the board
 * ships in. An empty slot costs the current the slot draws, which is
 * nothing, and nothing else on the board goes dark.
 */
static sd_pwr_ctrl_handle_t s_pwr;

static void sdcard_free(sdcard_t *s)
{
    if (s == NULL) return;
    if (s->lock) rv9_lock_destroy(s->lock);
    rv9_free(s->bounce);
    rv9_free(s->card);
    rv9_free(s);
}

static rv9_io_err_t sdcard_init(rv9_dev_t *dev)
{
    sdcard_t *s = rv9_calloc(1, sizeof(*s));
    if (s == NULL) return RV9_IO_ERR_NOMEM;

    s->card   = rv9_calloc(1, sizeof(sdmmc_card_t));
    s->bounce = rv9_alloc_dma(SECTOR_SIZE);
    if (s->card == NULL || s->bounce == NULL ||
        rv9_lock_create(&s->lock) != RV9_OK) {
        sdcard_free(s);
        return RV9_IO_ERR_NOMEM;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    /* Power first, or everything below finds an empty slot. Once only --
       see the note above on why this is never handed back. */
    if (s_pwr == NULL) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
            ESP_LOGE(TAG, "no LDO channel %d for the card slot", SD_LDO_CHAN);
            sdcard_free(s);
            return RV9_IO_ERR_IO;
        }
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = PIN_CLK;
    slot.cmd   = PIN_CMD;
    slot.d0    = PIN_D0;
    slot.d1    = PIN_D1;
    slot.d2    = PIN_D2;
    slot.d3    = PIN_D3;
    slot.width = SD_WIDTH;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (sdmmc_host_init() != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc host would not start");
        sdcard_free(s);
        return RV9_IO_ERR_IO;
    }

    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot) != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc slot would not start");
        sdcard_free(s);
        return RV9_IO_ERR_IO;
    }

    if (sdmmc_card_init(&host, s->card) != ESP_OK) {
        /* No card, or one that will not talk. Not an error worth shouting
           about: this board is often run without one. */
        ESP_LOGI(TAG, "%s: no card in the slot", dev->name);
        sdcard_free(s);
        return RV9_IO_ERR_NOTFOUND;
    }

    dev->drv_state = s;

    ESP_LOGI(TAG, "%s: card of %lu MB, %d-byte sectors, %d-bit at %d kHz",
             dev->name,
             (unsigned long)(((uint64_t)s->card->csd.capacity * SECTOR_SIZE) >> 20),
             s->card->csd.sector_size, SD_WIDTH, host.max_freq_khz);
    return RV9_IO_OK;
}

static rv9_io_err_t sdcard_geometry(rv9_dev_t *dev, uint32_t *sector_size,
                                    uint32_t *sector_count)
{
    sdcard_t *s = (sdcard_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    if (sector_size)  *sector_size  = SECTOR_SIZE;
    if (sector_count) *sector_count = (uint32_t)s->card->csd.capacity;
    return RV9_IO_OK;
}

static rv9_io_err_t sdcard_read(rv9_dev_t *dev, uint32_t lsn, void *buf,
                                uint32_t count)
{
    sdcard_t *s = (sdcard_t *)dev->drv_state;
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

static rv9_io_err_t sdcard_write(rv9_dev_t *dev, uint32_t lsn, const void *buf,
                                 uint32_t count)
{
    sdcard_t *s = (sdcard_t *)dev->drv_state;
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

static const rv9_driver_t sdcard = {
    .name         = "sdcard",
    .init         = sdcard_init,
    .geometry     = sdcard_geometry,
    .read_blocks  = sdcard_read,
    .write_blocks = sdcard_write,
};

rv9_io_err_t rv9_drv_sdcard_register(void)
{
    return rv9_io_register_driver(&sdcard);
}
