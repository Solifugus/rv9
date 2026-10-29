/*
 * flashdisk -- a block device backed by a flash partition.
 *
 * The point is persistence. A RAM disk loses every file and every loaded
 * program at reset, which is fine for proving a file manager works and
 * useless for anything real. This board has spare flash; that is a volume.
 *
 * The awkward part is that flash erases in 4 KB blocks and RBF writes in
 * 512-byte sectors. A partial write therefore means read the whole erase
 * block, patch the sectors inside it, erase, and write it back.
 *
 * This batches consecutive sectors that fall in one erase block, which
 * helps exactly when the caller offers several at once -- and RBF never
 * does. It writes a sector at a time, so every 512 bytes cost a 4 KB read,
 * a 4 KB erase and a 4 KB write. Measured: `mdir > /f0/b.txt`, five and a
 * half kilobytes, took **fifty-two seconds**. About a hundred bytes a
 * second, on a volume whose whole purpose is that programs live there.
 *
 * WHAT FLASH ACTUALLY REQUIRES
 *
 * An erase is only needed to turn a zero bit back into a one. Writing can
 * always clear bits. So a write whose every byte satisfies
 * (old & new) == new -- which includes the ordinary case of writing into
 * space that is still erased, all 0xFF -- can go straight to the flash
 * with no read-modify-erase cycle at all.
 *
 * That is the common case by a wide margin: appending to a file, filling a
 * fresh volume, writing a bitmap bit that only ever goes from one to zero.
 * The slow path remains for a genuine overwrite, which is what it was
 * always for.
 *
 * WHAT RBF ACTUALLY ASKS FOR
 *
 * That was not enough. Writing to /f0 still ran at about 10 KB/s once the
 * volume had been used, and a trace of copying 96 KB onto it said why: 567
 * sector writes and 556 erases for 188 sectors of file. Every data sector
 * was written twice, because `copy` moves 256 bytes at a time and RBF
 * writes each half through. The allocation bitmap's sector was written
 * after every one, and allocating sets a bit, which flash cannot do
 * without an erase. And space a deleted file left behind is not erased, so
 * the fast path above almost never applied.
 *
 * So the driver keeps the last few erase blocks it was asked to change in
 * memory, folds every sector written to them into one copy, and puts each
 * block back with one erase -- or none, if the fast path applies to the
 * whole block -- when it has not been touched for FLUSH_IDLE_MS or when
 * its slot is wanted for another block. Reads of a cached block come from
 * the cache, so nobody can see the flash behind it.
 *
 * The cost is durability: a write is in RAM for up to FLUSH_IDLE_MS
 * afterwards, and a power cut in that window loses it. That is the bargain
 * every file system with a cache makes, and the alternative was an erase
 * per 256 bytes. The flushing is done by a background task, below every
 * RV-9 thread, so on a machine that never goes idle it waits for an
 * eviction instead.
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

/* Blocks held in memory. Four on the P4, which has the room; two
   elsewhere, the fewest that keep the bitmap and the file being written
   from evicting each other on every sector. */
#if CONFIG_IDF_TARGET_ESP32P4
#define CACHE_SLOTS 4
#else
#define CACHE_SLOTS 2
#endif

#define FLUSH_IDLE_MS 50

typedef struct {
    uint32_t block;             /* erase block number */
    bool     valid, dirty;
    uint64_t touched_ms;        /* last write, or last use for eviction */
    uint8_t  data[ERASE_SIZE];
} slot_t;

typedef struct {
    const esp_partition_t *part;
    uint32_t               sectors;
    uint8_t                scratch[256];   /* a block compared a piece at a time */
    slot_t                 slot[CACHE_SLOTS];
    rv9_lock_t            lock;

    /* What the cache has saved, for anyone asking whether it works. */
    uint32_t               writes, flushes, erases;
} flashdisk_t;

/* ---- the cache, all under f->lock ---- */

/*
 * Put a dirty block back. Without an erase when every byte only clears
 * bits -- a block written into fresh space -- and with one otherwise.
 */
static rv9_io_err_t flush_slot(flashdisk_t *f, slot_t *sl)
{
    if (!sl->valid || !sl->dirty) return RV9_IO_OK;

    size_t off = (size_t)sl->block * ERASE_SIZE;
    bool clears_only = true, same = true;
    for (size_t at = 0; at < ERASE_SIZE && clears_only; at += sizeof(f->scratch)) {
        if (esp_partition_read(f->part, off + at, f->scratch,
                               sizeof(f->scratch)) != ESP_OK) {
            clears_only = false;
            break;
        }
        const uint8_t *want = sl->data + at;
        for (size_t i = 0; i < sizeof(f->scratch); i++) {
            if ((f->scratch[i] & want[i]) != want[i]) { clears_only = false; break; }
            if (f->scratch[i] != want[i]) same = false;
        }
    }
    if (clears_only && same) {
        sl->dirty = false;               /* nothing changed after all */
        return RV9_IO_OK;
    }

    if (!clears_only) {
        if (esp_partition_erase_range(f->part, off, ERASE_SIZE) != ESP_OK) {
            return RV9_IO_ERR_IO;
        }
        f->erases++;
    }
    if (esp_partition_write(f->part, off, sl->data, ERASE_SIZE) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }
    f->flushes++;
    sl->dirty = false;
    return RV9_IO_OK;
}

static slot_t *find_slot(flashdisk_t *f, uint32_t block)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (f->slot[i].valid && f->slot[i].block == block) return &f->slot[i];
    }
    return NULL;
}

/* A slot holding `block`, loading it -- and evicting the least recently
   used, writing it back first if it is dirty -- when it is not there. */
static rv9_io_err_t get_slot(flashdisk_t *f, uint32_t block, slot_t **out)
{
    slot_t *sl = find_slot(f, block);
    if (sl == NULL) {
        sl = &f->slot[0];
        for (int i = 0; i < CACHE_SLOTS; i++) {
            slot_t *c = &f->slot[i];
            if (!c->valid) { sl = c; break; }
            if (c->touched_ms < sl->touched_ms) sl = c;
        }
        rv9_io_err_t err = flush_slot(f, sl);
        if (err != RV9_IO_OK) return err;

        sl->valid = false;
        if (esp_partition_read(f->part, (size_t)block * ERASE_SIZE, sl->data,
                               ERASE_SIZE) != ESP_OK) {
            return RV9_IO_ERR_IO;
        }
        sl->block = block;
        sl->valid = true;
        sl->dirty = false;
    }
    sl->touched_ms = rv9_time_ms();
    *out = sl;
    return RV9_IO_OK;
}

static void flusher(void *arg)
{
    flashdisk_t *f = (flashdisk_t *)arg;
    for (;;) {
        rv9_task_delay_ms(FLUSH_IDLE_MS / 2);
        rv9_lock_acquire(f->lock);
        uint64_t now = rv9_time_ms();
        for (int i = 0; i < CACHE_SLOTS; i++) {
            slot_t *sl = &f->slot[i];
            if (sl->dirty && now - sl->touched_ms >= FLUSH_IDLE_MS) {
                if (flush_slot(f, sl) != RV9_IO_OK) {
                    ESP_LOGE(TAG, "block %lu could not be written back",
                             (unsigned long)sl->block);
                }
            }
        }
        rv9_lock_release(f->lock);
    }
}

/* ---- the driver ---- */

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

    if (rv9_lock_create(&f->lock) != RV9_OK) {
        rv9_free(f);
        return RV9_IO_ERR_NOMEM;
    }

    /* Without something to write the cache back there is no cache; the
       volume is still usable, one erase per write as it used to be. */
    if (rv9_task_create_background(flusher, "rv9-flashdisk", 3072, f)
            != RV9_OK) {
        rv9_free(f);
        return RV9_IO_ERR_NOMEM;
    }

    f->sectors = f->part->size / SECTOR_SIZE;
    dev->drv_state = f;

    ESP_LOGI(TAG, "%lu sectors of %d bytes (%lu KB) on '%s', %d blocks cached",
             (unsigned long)f->sectors, SECTOR_SIZE,
             (unsigned long)(f->part->size / 1024), PARTITION_LABEL,
             CACHE_SLOTS);
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

    uint8_t *dst = (uint8_t *)buf;
    rv9_io_err_t result = RV9_IO_OK;

    /* Direct, a run at a time, except where a cached block stands in for
       the flash behind it. */
    rv9_lock_acquire(f->lock);
    uint32_t done = 0;
    while (done < count) {
        uint32_t sector = lsn + done;
        slot_t  *sl = find_slot(f, sector / SECTORS_PER_ERASE);
        if (sl != NULL) {
            memcpy(dst + (size_t)done * SECTOR_SIZE,
                   sl->data + (sector % SECTORS_PER_ERASE) * SECTOR_SIZE,
                   SECTOR_SIZE);
            done++;
            continue;
        }
        uint32_t run = 1;
        while (done + run < count &&
               find_slot(f, (sector + run) / SECTORS_PER_ERASE) == NULL) {
            run++;
        }
        if (esp_partition_read(f->part, (size_t)sector * SECTOR_SIZE,
                               dst + (size_t)done * SECTOR_SIZE,
                               (size_t)run * SECTOR_SIZE) != ESP_OK) {
            result = RV9_IO_ERR_IO;
            break;
        }
        done += run;
    }
    rv9_lock_release(f->lock);
    return result;
}

static rv9_io_err_t flashdisk_write(rv9_dev_t *dev, uint32_t lsn,
                                    const void *buf, uint32_t count)
{
    flashdisk_t *f = (flashdisk_t *)dev->drv_state;
    if (f == NULL) return RV9_IO_ERR_IO;
    if (lsn + count > f->sectors) return RV9_IO_ERR_INVAL;

    const uint8_t *src = (const uint8_t *)buf;
    rv9_io_err_t result = RV9_IO_OK;

    rv9_lock_acquire(f->lock);
    for (uint32_t done = 0; done < count; done++) {
        uint32_t sector = lsn + done;
        slot_t  *sl = NULL;
        result = get_slot(f, sector / SECTORS_PER_ERASE, &sl);
        if (result != RV9_IO_OK) break;
        memcpy(sl->data + (sector % SECTORS_PER_ERASE) * SECTOR_SIZE,
               src + (size_t)done * SECTOR_SIZE, SECTOR_SIZE);
        sl->dirty = true;
        f->writes++;
    }
    rv9_lock_release(f->lock);
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
