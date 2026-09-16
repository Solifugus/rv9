/*
 * RBF -- random block file manager.
 *
 * The structure follows the classic OS-9 RBF; the encodings are RV-9's own:
 *
 *   LSN 0        identification sector: geometry and where everything is
 *   LSN 1..      allocation bitmap, one bit per sector
 *   root dir     fixed-size entries, name plus file descriptor sector
 *   fd sector    one per file: size, and a list of segments
 *   segments     (start sector, count) pairs -- a file is a list of runs,
 *                not a chain of blocks, so contiguous files cost one entry
 *
 * The segment list is the part worth keeping. FAT-style chains make you
 * walk the whole file to find the end; a segment list finds any offset in
 * a handful of comparisons and stays small when files are not fragmented.
 *
 * A directory is a file, so opening "/r0" rather than "/r0/notes" reads
 * directory entries. That falls out of the design rather than being a
 * special case bolted on.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-rbf";

#define RBF_MAGIC       "RV9RBF\0"
#define RBF_MAGIC_LEN   8
#define SECTOR_SIZE     512
#define MAX_SEGMENTS    62
#define DIR_ENTRIES_PER_SECTOR (SECTOR_SIZE / (int)sizeof(rbf_dirent_t))

/* On-media identification sector. */
typedef struct __attribute__((packed)) {
    char     magic[RBF_MAGIC_LEN];
    uint32_t total_sectors;
    uint16_t sector_size;
    uint16_t reserved;
    uint32_t bitmap_lsn;
    uint32_t bitmap_sectors;
    uint32_t root_lsn;
    uint32_t root_sectors;
    char     volume[16];
} rbf_ident_t;

/* On-media directory entry. */
typedef struct __attribute__((packed)) {
    char     name[28];
    uint32_t fd_lsn;        /* 0 means the slot is free */
} rbf_dirent_t;

/* On-media file descriptor sector. */
typedef struct __attribute__((packed)) {
    uint32_t size;          /* bytes */
    uint16_t segment_count;
    uint16_t reserved;
    struct __attribute__((packed)) {
        uint32_t lsn;
        uint32_t count;
    } seg[MAX_SEGMENTS];
} rbf_fd_t;

_Static_assert(sizeof(rbf_fd_t) <= SECTOR_SIZE, "fd must fit one sector");
_Static_assert(sizeof(rbf_dirent_t) == 32, "dirent must be 32 bytes");

/* Per-device mount state. */
typedef struct {
    rbf_ident_t ident;
    uint32_t    alloc_hint;      /* bitmap sector the last allocation came from */
    uint32_t    sector_count;
    bool        mounted;
    rv9_lock_t lock;
    uint8_t     scratch[SECTOR_SIZE];   /* one shared bounce buffer */
} rbf_mount_t;

/* Per-path state. */
typedef struct {
    bool      is_dir;
    uint32_t  fd_lsn;
    rbf_fd_t  fd;
    bool      dirty;
} rbf_path_t;

/* ------------------------------------------------------------------ */
/* Sector I/O                                                          */
/* ------------------------------------------------------------------ */

static rv9_io_err_t rd(rv9_dev_t *dev, uint32_t lsn, void *buf)
{
    if (dev->drv->read_blocks == NULL) return RV9_IO_ERR_UNSUPPORTED;
    return dev->drv->read_blocks(dev, lsn, buf, 1);
}

static rv9_io_err_t wr(rv9_dev_t *dev, uint32_t lsn, const void *buf)
{
    if (dev->drv->write_blocks == NULL) return RV9_IO_ERR_UNSUPPORTED;
    return dev->drv->write_blocks(dev, lsn, buf, 1);
}

/* ------------------------------------------------------------------ */
/* Allocation bitmap                                                   */
/* ------------------------------------------------------------------ */

static rv9_io_err_t bitmap_set(rv9_dev_t *dev, rbf_mount_t *m, uint32_t lsn,
                               bool used)
{
    uint32_t byte = lsn / 8;
    uint32_t sector = m->ident.bitmap_lsn + byte / SECTOR_SIZE;
    uint32_t offset = byte % SECTOR_SIZE;

    rv9_io_err_t err = rd(dev, sector, m->scratch);
    if (err != RV9_IO_OK) return err;

    uint8_t mask = (uint8_t)(1u << (lsn % 8));
    if (used) m->scratch[offset] |= mask;
    else      m->scratch[offset] &= (uint8_t)~mask;

    return wr(dev, sector, m->scratch);
}

static bool bitmap_get(rv9_dev_t *dev, rbf_mount_t *m, uint32_t lsn)
{
    uint32_t byte = lsn / 8;
    uint32_t sector = m->ident.bitmap_lsn + byte / SECTOR_SIZE;
    uint32_t offset = byte % SECTOR_SIZE;

    if (rd(dev, sector, m->scratch) != RV9_IO_OK) return true;
    return (m->scratch[offset] & (1u << (lsn % 8))) != 0;
}

/* First free sector, or 0 if the volume is full. LSN 0 is the identification
   sector, so 0 is never a valid allocation and doubles as "none". */
/*
 * A free sector, found by reading the bitmap rather than the volume.
 *
 * This used to ask bitmap_get() about one sector at a time, and each of
 * those questions read a whole bitmap sector from the device. On a RAM
 * disk that is invisible. On a 32 GB card, whose first 15,221 sectors are
 * metadata, creating the first file read the same bitmap sector fifteen
 * thousand times: nineteen seconds, measured, for one empty file.
 *
 * So the scan is over bitmap sectors: read one, look at its bits, take the
 * first that is clear and write the sector back with that bit set. Four
 * reads instead of fifteen thousand. The hint remembers which bitmap
 * sector the last allocation came from, so a volume filling up does not
 * start from the beginning every time; it wraps, so nothing is missed.
 */
static uint32_t alloc_sector(rv9_dev_t *dev, rbf_mount_t *m)
{
    const uint32_t bits_per_sector = SECTOR_SIZE * 8;
    uint32_t maps = m->ident.bitmap_sectors;
    if (maps == 0) return 0;

    uint32_t start = (m->alloc_hint < maps) ? m->alloc_hint : 0;

    for (uint32_t n = 0; n < maps; n++) {
        uint32_t map = (start + n) % maps;

        if (rd(dev, m->ident.bitmap_lsn + map, m->scratch) != RV9_IO_OK) {
            return 0;
        }

        for (uint32_t byte = 0; byte < SECTOR_SIZE; byte++) {
            if (m->scratch[byte] == 0xFF) continue;

            for (uint32_t bit = 0; bit < 8; bit++) {
                if (m->scratch[byte] & (1u << bit)) continue;

                uint32_t lsn = map * bits_per_sector + byte * 8 + bit;
                if (lsn == 0) continue;                    /* the ident sector */
                if (lsn >= m->ident.total_sectors) return 0;   /* past the end */

                m->scratch[byte] |= (uint8_t)(1u << bit);
                if (wr(dev, m->ident.bitmap_lsn + map, m->scratch) != RV9_IO_OK) {
                    return 0;
                }
                m->alloc_hint = map;
                return lsn;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Format and mount                                                    */
/* ------------------------------------------------------------------ */

static rv9_io_err_t rbf_format(rv9_dev_t *dev, rbf_mount_t *m,
                               uint32_t sectors)
{
    uint32_t bitmap_sectors = (sectors / 8 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (bitmap_sectors == 0) bitmap_sectors = 1;

    /*
     * The root directory, sized to the volume it is on.
     *
     * Two sectors is 32 files, which is right for a 16 KB RAM disk and
     * absurd on a 32 GB card. Every sector of it is read when a name is
     * looked up and not found, so this is a trade rather than a maximum to
     * be raised freely: sixteen sectors is 256 files, and sixteen reads for
     * a miss is about twenty milliseconds on a card.
     */
    uint32_t root_sectors = 2;                              /* 32 files */
    if (sectors >= (1u << 20))      root_sectors = 16;      /* 256 files */
    else if (sectors >= (1u << 16)) root_sectors = 8;       /* 128 files */

    memset(&m->ident, 0, sizeof(m->ident));
    memcpy(m->ident.magic, RBF_MAGIC, RBF_MAGIC_LEN);
    m->ident.total_sectors  = sectors;
    m->ident.sector_size    = SECTOR_SIZE;
    m->ident.bitmap_lsn     = 1;
    m->ident.bitmap_sectors = bitmap_sectors;
    m->ident.root_lsn       = 1 + bitmap_sectors;
    m->ident.root_sectors   = root_sectors;
    strncpy(m->ident.volume, "rv9", sizeof(m->ident.volume) - 1);

    /* Zero the bitmap and the directory. */
    memset(m->scratch, 0, SECTOR_SIZE);
    for (uint32_t i = 0; i < bitmap_sectors; i++) {
        rv9_io_err_t err = wr(dev, m->ident.bitmap_lsn + i, m->scratch);
        if (err != RV9_IO_OK) return err;
    }
    for (uint32_t i = 0; i < root_sectors; i++) {
        rv9_io_err_t err = wr(dev, m->ident.root_lsn + i, m->scratch);
        if (err != RV9_IO_OK) return err;
    }

    /* Write the identification sector before marking anything used, so a
       half-formatted volume fails its magic check rather than looking
       plausible. */
    memset(m->scratch, 0, SECTOR_SIZE);
    memcpy(m->scratch, &m->ident, sizeof(m->ident));
    rv9_io_err_t err = wr(dev, 0, m->scratch);
    if (err != RV9_IO_OK) return err;

    /* Reserve the metadata sectors. */
    uint32_t reserved = m->ident.root_lsn + root_sectors;
    for (uint32_t lsn = 0; lsn < reserved; lsn++) {
        err = bitmap_set(dev, m, lsn, true);
        if (err != RV9_IO_OK) return err;
    }

    ESP_LOGI(TAG, "%s formatted: %lu sectors, bitmap at %lu, root at %lu",
             dev->name, (unsigned long)sectors,
             (unsigned long)m->ident.bitmap_lsn,
             (unsigned long)m->ident.root_lsn);
    return RV9_IO_OK;
}

static rv9_io_err_t rbf_mount(rv9_dev_t *dev)
{
    if (dev->drv->geometry == NULL) {
        ESP_LOGE(TAG, "%s: driver '%s' is not a block device",
                 dev->name, dev->drv->name);
        return RV9_IO_ERR_UNSUPPORTED;
    }

    rbf_mount_t *m = rv9_calloc(1, sizeof(*m));
    if (m == NULL) return RV9_IO_ERR_NOMEM;
    if (rv9_lock_create(&m->lock) != RV9_OK) {
        rv9_free(m);
        return RV9_IO_ERR_NOMEM;
    }

    uint32_t sector_size = 0;
    rv9_io_err_t err = dev->drv->geometry(dev, &sector_size, &m->sector_count);
    if (err != RV9_IO_OK) { rv9_free(m); return err; }

    if (sector_size != SECTOR_SIZE) {
        ESP_LOGE(TAG, "%s: %lu-byte sectors, RBF wants %d",
                 dev->name, (unsigned long)sector_size, SECTOR_SIZE);
        rv9_free(m);
        return RV9_IO_ERR_UNSUPPORTED;
    }

    err = rd(dev, 0, m->scratch);
    if (err != RV9_IO_OK) { rv9_free(m); return err; }
    memcpy(&m->ident, m->scratch, sizeof(m->ident));

    if (memcmp(m->ident.magic, RBF_MAGIC, RBF_MAGIC_LEN) != 0) {
        /* Unformatted. A RAM disk is empty every boot, so formatting on
           sight is right here; a real card should not be touched without
           being asked. */
        ESP_LOGI(TAG, "%s has no RBF volume, formatting", dev->name);
        err = rbf_format(dev, m, m->sector_count);
        if (err != RV9_IO_OK) { rv9_free(m); return err; }
    }

    m->mounted = true;
    dev->fmgr_state = m;

    ESP_LOGI(TAG, "%s mounted: volume '%s', %lu sectors",
             dev->name, m->ident.volume,
             (unsigned long)m->ident.total_sectors);
    return RV9_IO_OK;
}

/* ------------------------------------------------------------------ */
/* Directory                                                           */
/* ------------------------------------------------------------------ */

/* Find `name`, or the first free slot if `name` is NULL. Returns the entry
   index, or -1. */
static int dir_find(rv9_dev_t *dev, rbf_mount_t *m, const char *name,
                    rbf_dirent_t *out)
{
    for (uint32_t s = 0; s < m->ident.root_sectors; s++) {
        if (rd(dev, m->ident.root_lsn + s, m->scratch) != RV9_IO_OK) return -1;

        rbf_dirent_t *ents = (rbf_dirent_t *)m->scratch;
        for (int i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
            bool match = name ? (ents[i].fd_lsn != 0 &&
                                 strncmp(ents[i].name, name, 27) == 0)
                              : (ents[i].fd_lsn == 0);
            if (match) {
                if (out) *out = ents[i];
                return (int)(s * DIR_ENTRIES_PER_SECTOR + i);
            }
        }
    }
    return -1;
}

static rv9_io_err_t dir_write(rv9_dev_t *dev, rbf_mount_t *m, int index,
                              const rbf_dirent_t *ent)
{
    uint32_t s = (uint32_t)index / DIR_ENTRIES_PER_SECTOR;
    int i = index % DIR_ENTRIES_PER_SECTOR;

    rv9_io_err_t err = rd(dev, m->ident.root_lsn + s, m->scratch);
    if (err != RV9_IO_OK) return err;

    ((rbf_dirent_t *)m->scratch)[i] = *ent;
    return wr(dev, m->ident.root_lsn + s, m->scratch);
}

/* ------------------------------------------------------------------ */
/* File descriptors and segments                                       */
/* ------------------------------------------------------------------ */

static rv9_io_err_t fd_read(rv9_dev_t *dev, uint32_t lsn, rbf_fd_t *fd)
{
    uint8_t buf[SECTOR_SIZE];
    rv9_io_err_t err = rd(dev, lsn, buf);
    if (err != RV9_IO_OK) return err;
    memcpy(fd, buf, sizeof(*fd));
    return RV9_IO_OK;
}

static rv9_io_err_t fd_write(rv9_dev_t *dev, uint32_t lsn, const rbf_fd_t *fd)
{
    uint8_t buf[SECTOR_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, fd, sizeof(*fd));
    return wr(dev, lsn, buf);
}

/* Map a byte offset to the sector holding it. */
static uint32_t offset_to_lsn(const rbf_fd_t *fd, uint32_t offset)
{
    uint32_t want = offset / SECTOR_SIZE;

    for (uint16_t i = 0; i < fd->segment_count; i++) {
        if (want < fd->seg[i].count) return fd->seg[i].lsn + want;
        want -= fd->seg[i].count;
    }
    return 0;
}

/* Add one sector to the file, extending the last segment if it happens to
   be contiguous -- which it usually is, and which is why a segment list
   stays short. */
static rv9_io_err_t fd_grow(rv9_dev_t *dev, rbf_mount_t *m, rbf_fd_t *fd)
{
    uint32_t lsn = alloc_sector(dev, m);
    if (lsn == 0) return RV9_IO_ERR_IO;

    if (fd->segment_count > 0) {
        uint16_t last = (uint16_t)(fd->segment_count - 1);
        if (fd->seg[last].lsn + fd->seg[last].count == lsn) {
            fd->seg[last].count++;
            return RV9_IO_OK;
        }
    }

    if (fd->segment_count >= MAX_SEGMENTS) {
        bitmap_set(dev, m, lsn, false);
        return RV9_IO_ERR_IO;      /* too fragmented to describe */
    }

    fd->seg[fd->segment_count].lsn   = lsn;
    fd->seg[fd->segment_count].count = 1;
    fd->segment_count++;
    return RV9_IO_OK;
}

static void fd_release(rv9_dev_t *dev, rbf_mount_t *m, const rbf_fd_t *fd)
{
    for (uint16_t i = 0; i < fd->segment_count; i++) {
        for (uint32_t j = 0; j < fd->seg[i].count; j++) {
            bitmap_set(dev, m, fd->seg[i].lsn + j, false);
        }
    }
}

/* ------------------------------------------------------------------ */
/* File manager entry points                                           */
/* ------------------------------------------------------------------ */

static rv9_io_err_t rbf_open(rv9_path_t *path, const char *rest)
{
    rv9_dev_t *dev = path->dev;
    rbf_mount_t *m = (rbf_mount_t *)dev->fmgr_state;
    if (m == NULL || !m->mounted) return RV9_IO_ERR_IO;

    rbf_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;

    /* No filename means the directory itself. */
    if (rest == NULL || rest[0] == '\0') {
        st->is_dir = true;
        path->fm_state = st;
        return RV9_IO_OK;
    }

    rv9_lock_acquire(m->lock);

    rbf_dirent_t ent;
    int index = dir_find(dev, m, rest, &ent);
    rv9_io_err_t err = RV9_IO_OK;

    if (index >= 0 && !(path->mode & RV9_MODE_CREATE)) {
        st->fd_lsn = ent.fd_lsn;
        err = fd_read(dev, st->fd_lsn, &st->fd);

    } else if (index >= 0) {
        /* Exists and CREATE was asked for: truncate it. */
        st->fd_lsn = ent.fd_lsn;
        err = fd_read(dev, st->fd_lsn, &st->fd);
        if (err == RV9_IO_OK) {
            fd_release(dev, m, &st->fd);
            memset(&st->fd, 0, sizeof(st->fd));
            st->dirty = true;
        }

    } else if (path->mode & RV9_MODE_CREATE) {
        int slot = dir_find(dev, m, NULL, NULL);
        if (slot < 0) {
            err = RV9_IO_ERR_IO;          /* directory full */
        } else {
            uint32_t fd_lsn = alloc_sector(dev, m);
            if (fd_lsn == 0) {
                err = RV9_IO_ERR_IO;      /* volume full */
            } else {
                memset(&st->fd, 0, sizeof(st->fd));
                st->fd_lsn = fd_lsn;
                st->dirty = true;

                memset(&ent, 0, sizeof(ent));
                strncpy(ent.name, rest, sizeof(ent.name) - 1);
                ent.fd_lsn = fd_lsn;
                err = dir_write(dev, m, slot, &ent);
                if (err == RV9_IO_OK) err = fd_write(dev, fd_lsn, &st->fd);
            }
        }
    } else {
        err = RV9_IO_ERR_NOTFOUND;
    }

    rv9_lock_release(m->lock);

    if (err != RV9_IO_OK) {
        rv9_free(st);
        return err;
    }

    strncpy(path->name, rest, sizeof(path->name) - 1);
    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t rbf_close(rv9_path_t *path)
{
    rbf_path_t *st = (rbf_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_OK;

    if (st->dirty && !st->is_dir) {
        rbf_mount_t *m = (rbf_mount_t *)path->dev->fmgr_state;
        rv9_lock_acquire(m->lock);
        fd_write(path->dev, st->fd_lsn, &st->fd);
        rv9_lock_release(m->lock);
    }

    rv9_free(st);
    path->fm_state = NULL;
    return RV9_IO_OK;
}

/* Reading the device itself walks the directory. */
static rv9_io_err_t dir_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    rv9_dev_t *dev = path->dev;
    rbf_mount_t *m = (rbf_mount_t *)dev->fmgr_state;

    uint32_t want = (uint32_t)(len / sizeof(rv9_dirent_t));
    if (want == 0) return RV9_IO_ERR_INVAL;

    rv9_dirent_t *out = (rv9_dirent_t *)buf;
    uint32_t produced = 0;
    uint32_t skip = (uint32_t)(path->pos / (int64_t)sizeof(rv9_dirent_t));
    uint32_t seen = 0;

    rv9_lock_acquire(m->lock);

    for (uint32_t s = 0; s < m->ident.root_sectors && produced < want; s++) {
        if (rd(dev, m->ident.root_lsn + s, m->scratch) != RV9_IO_OK) break;

        rbf_dirent_t *ents = (rbf_dirent_t *)m->scratch;
        for (int i = 0; i < DIR_ENTRIES_PER_SECTOR && produced < want; i++) {
            if (ents[i].fd_lsn == 0) continue;
            if (seen++ < skip) continue;

            rbf_fd_t fd;
            uint32_t size = 0;
            if (fd_read(dev, ents[i].fd_lsn, &fd) == RV9_IO_OK) size = fd.size;

            memset(&out[produced], 0, sizeof(out[produced]));
            strncpy(out[produced].name, ents[i].name,
                    sizeof(out[produced].name) - 1);
            out[produced].size = size;
            produced++;
        }
    }

    rv9_lock_release(m->lock);

    path->pos += (int64_t)produced * (int64_t)sizeof(rv9_dirent_t);
    if (done) *done = produced * sizeof(rv9_dirent_t);
    return RV9_IO_OK;
}

static rv9_io_err_t rbf_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    rbf_path_t *st = (rbf_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_ERR_IO;
    if (st->is_dir) return dir_read(path, buf, len, done);

    rv9_dev_t *dev = path->dev;
    rbf_mount_t *m = (rbf_mount_t *)dev->fmgr_state;

    uint32_t pos = (uint32_t)path->pos;
    if (pos >= st->fd.size) { if (done) *done = 0; return RV9_IO_OK; }

    uint32_t remain = st->fd.size - pos;
    if (len > remain) len = remain;

    uint8_t *out = (uint8_t *)buf;
    size_t moved = 0;

    rv9_lock_acquire(m->lock);

    while (moved < len) {
        uint32_t lsn = offset_to_lsn(&st->fd, pos);
        if (lsn == 0) break;

        uint32_t within = pos % SECTOR_SIZE;
        uint32_t chunk = SECTOR_SIZE - within;
        if (chunk > len - moved) chunk = (uint32_t)(len - moved);

        if (rd(dev, lsn, m->scratch) != RV9_IO_OK) break;
        memcpy(out + moved, m->scratch + within, chunk);

        moved += chunk;
        pos   += chunk;
    }

    rv9_lock_release(m->lock);

    path->pos = pos;
    if (done) *done = moved;
    return RV9_IO_OK;
}

static rv9_io_err_t rbf_write(rv9_path_t *path, const void *buf, size_t len,
                              size_t *done)
{
    rbf_path_t *st = (rbf_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_ERR_IO;
    if (st->is_dir) return RV9_IO_ERR_UNSUPPORTED;

    rv9_dev_t *dev = path->dev;
    rbf_mount_t *m = (rbf_mount_t *)dev->fmgr_state;

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t pos = (uint32_t)path->pos;
    size_t moved = 0;

    rv9_lock_acquire(m->lock);

    while (moved < len) {
        uint32_t needed = (pos + SECTOR_SIZE) / SECTOR_SIZE;
        uint32_t have = 0;
        for (uint16_t i = 0; i < st->fd.segment_count; i++) {
            have += st->fd.seg[i].count;
        }

        while (have < needed) {
            if (fd_grow(dev, m, &st->fd) != RV9_IO_OK) goto out;
            have++;
            st->dirty = true;
        }

        uint32_t lsn = offset_to_lsn(&st->fd, pos);
        if (lsn == 0) break;

        uint32_t within = pos % SECTOR_SIZE;
        uint32_t chunk = SECTOR_SIZE - within;
        if (chunk > len - moved) chunk = (uint32_t)(len - moved);

        /* Read-modify-write when only part of the sector changes. */
        if (within != 0 || chunk != SECTOR_SIZE) {
            if (rd(dev, lsn, m->scratch) != RV9_IO_OK) break;
        }
        memcpy(m->scratch + within, in + moved, chunk);
        if (wr(dev, lsn, m->scratch) != RV9_IO_OK) break;

        moved += chunk;
        pos   += chunk;
    }

out:
    if (pos > st->fd.size) { st->fd.size = pos; st->dirty = true; }
    rv9_lock_release(m->lock);

    path->pos = pos;
    if (done) *done = moved;
    return moved > 0 || len == 0 ? RV9_IO_OK : RV9_IO_ERR_IO;
}

static rv9_io_err_t rbf_seek(rv9_path_t *path, int64_t offset, int whence)
{
    rbf_path_t *st = (rbf_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_ERR_IO;

    int64_t base;
    switch (whence) {
    case RV9_SEEK_SET: base = 0; break;
    case RV9_SEEK_CUR: base = path->pos; break;
    case RV9_SEEK_END: base = st->is_dir ? 0 : (int64_t)st->fd.size; break;
    default: return RV9_IO_ERR_INVAL;
    }

    int64_t target = base + offset;
    if (target < 0) return RV9_IO_ERR_INVAL;

    path->pos = target;
    return RV9_IO_OK;
}

static rv9_io_err_t rbf_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    rbf_path_t *st = (rbf_path_t *)path->fm_state;

    if (code == RV9_GS_SIZE && arg && st && !st->is_dir) {
        *(uint64_t *)arg = st->fd.size;
        return RV9_IO_OK;
    }

    /*
     * Room left, counted by reading the bitmap: one pass over
     * bitmap_sectors, not over sectors. On this card that is fifteen
     * thousand sectors of bitmap for sixty-two million sectors of volume,
     * so counting the bits is the only affordable way to do it.
     */
    if (code == RV9_RBF_GS_SPACE && arg && st && st->is_dir) {
        rv9_dev_t   *dev = path->dev;
        rbf_mount_t *m   = (rbf_mount_t *)dev->fmgr_state;
        if (m == NULL || !m->mounted) return RV9_IO_ERR_IO;

        rv9_rbf_space_t *out = (rv9_rbf_space_t *)arg;
        uint32_t used = 0;

        rv9_lock_acquire(m->lock);
        for (uint32_t map = 0; map < m->ident.bitmap_sectors; map++) {
            if (rd(dev, m->ident.bitmap_lsn + map, m->scratch) != RV9_IO_OK) {
                rv9_lock_release(m->lock);
                return RV9_IO_ERR_IO;
            }
            for (uint32_t byte = 0; byte < SECTOR_SIZE; byte++) {
                uint8_t v = m->scratch[byte];
                while (v) { used += (v & 1u); v >>= 1; }
            }
        }
        rv9_lock_release(m->lock);

        out->sector_size   = SECTOR_SIZE;
        out->total_sectors = m->ident.total_sectors;
        out->free_sectors  = (used < m->ident.total_sectors)
                             ? m->ident.total_sectors - used : 0;
        return RV9_IO_OK;
    }

    return RV9_IO_ERR_UNSUPPORTED;
}

/*
 * Make an empty volume here, destroying what is on it.
 *
 * Only on the device itself -- `/sd0`, not `/sd0/notes` -- because a format
 * is about the volume, and asking for one through a file would be an odd
 * way to say it. Mounting formats an unrecognised volume on sight, which
 * suits a RAM disk that starts empty every boot; this is for the other
 * case, where the volume is recognised and is to be emptied anyway.
 */
static rv9_io_err_t rbf_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    (void)arg;

    if (code != RV9_RBF_SS_FORMAT) return RV9_IO_ERR_UNSUPPORTED;

    rbf_path_t  *st  = (rbf_path_t *)path->fm_state;
    rv9_dev_t   *dev = path->dev;
    rbf_mount_t *m   = (rbf_mount_t *)dev->fmgr_state;

    if (st == NULL || !st->is_dir) return RV9_IO_ERR_INVAL;
    if (m == NULL) return RV9_IO_ERR_IO;

    rv9_lock_acquire(m->lock);
    m->mounted    = false;
    m->alloc_hint = 0;
    rv9_io_err_t err = rbf_format(dev, m, m->sector_count);
    m->mounted = (err == RV9_IO_OK);
    rv9_lock_release(m->lock);

    if (err == RV9_IO_OK) {
        ESP_LOGW(TAG, "%s formatted on request: everything on it is gone",
                 dev->name);
    }
    return err;
}

static rv9_io_err_t rbf_remove(rv9_dev_t *dev, const char *name)
{
    rbf_mount_t *m = (rbf_mount_t *)dev->fmgr_state;
    if (m == NULL || name == NULL || name[0] == '\0') return RV9_IO_ERR_INVAL;

    rv9_lock_acquire(m->lock);

    rbf_dirent_t ent;
    int index = dir_find(dev, m, name, &ent);
    rv9_io_err_t err = RV9_IO_ERR_NOTFOUND;

    if (index >= 0) {
        rbf_fd_t fd;
        if (fd_read(dev, ent.fd_lsn, &fd) == RV9_IO_OK) {
            fd_release(dev, m, &fd);
        }
        bitmap_set(dev, m, ent.fd_lsn, false);

        memset(&ent, 0, sizeof(ent));
        err = dir_write(dev, m, index, &ent);
    }

    rv9_lock_release(m->lock);
    return err;
}

static const rv9_filemgr_t rbf = {
    .name    = "rbf",
    .mount   = rbf_mount,
    .open    = rbf_open,
    .close   = rbf_close,
    .read    = rbf_read,
    .write   = rbf_write,
    .seek    = rbf_seek,
    .getstat = rbf_getstat,
    .setstat = rbf_setstat,
    .remove  = rbf_remove,
};

rv9_io_err_t rv9_rbf_register(void)
{
    return rv9_io_register_filemgr(&rbf);
}
