/*
 * RV-9 module directory and loader.
 *
 * Phase 1. Modules live in a flash partition, are verified by CRC, indexed
 * into an in-RAM directory at boot, and loaded on demand into executable
 * memory. Link counting means one image serves many callers -- which is
 * only safe because module code is reentrant by construction (see
 * rv9/module.h).
 */
#include "rv9/module.h"
#include "rv9/kal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sig.h"
#include "esp_partition.h"

static const char *TAG = "rv9-mod";

/*
 * Where the kept log comes from.
 *
 * Registered rather than called, because the ring lives above this layer
 * -- it is part of the system rv9_module serves, not part of the module
 * manager. A build without one answers RV9_SYS_LOG with nothing, which is
 * the truth for that build.
 */
static uint32_t (*s_log_read)(uint32_t from, char *out, uint32_t cap);
static uint32_t (*s_log_held)(void);
static bool     (*s_clock_read)(uint32_t *epoch, uint32_t *age_s);

void rv9_mod_set_clock_source(bool (*read)(uint32_t *, uint32_t *))
{
    s_clock_read = read;
}

void rv9_mod_set_log_source(uint32_t (*read)(uint32_t, char *, uint32_t),
                            uint32_t (*held)(void))
{
    s_log_read = read;
    s_log_held = held;
}

/*
 * Storage backend.
 *
 * TODO (phase 5): this belongs behind an RBF driver once the I/O manager
 * exists. Modules should come from any block device, not just this one
 * partition. Kept deliberately small so that swap is easy.
 */
#define RV9_MODULE_PARTITION_LABEL "modules"

static const esp_partition_t *s_store;

/*
 * The store, mapped into the address space so modules can run where they
 * lie instead of being copied into RAM.
 *
 * SPIKE, 2026-09-23. The whole of "linking" a module is a memcpy and a
 * fence -- no relocation, no fixups, because position independence was paid
 * for at build time with -mcmodel=medany. So the copy is not doing anything
 * except spending RAM: 9,348 bytes of it for the shell's code alone.
 *
 * What it buys is determinism. Executing from mapped flash goes through the
 * 32 KB cache, so a miss or a flash write elsewhere stalls the fetch. That
 * is exactly the trade RV-9 already makes for its own code with
 * RV9_RT_CODE, and it lands in the right place: an interactive program can
 * afford a cache miss and a control loop cannot.
 *
 * Mapped once, never unmapped: the mapping is the store, not a module.
 */
static const void *s_store_map;
static esp_partition_mmap_handle_t s_store_mmap;
static bool s_xip;
static rv9_mod_entry_t       *s_dir;
static rv9_lock_t            s_lock;

const char *rv9_mod_strerror(rv9_mod_err_t err)
{
    switch (err) {
    case RV9_MOD_OK:           return "ok";
    case RV9_MOD_ERR_NOTFOUND: return "no such module";
    case RV9_MOD_ERR_BADMAGIC: return "bad magic";
    case RV9_MOD_ERR_BADCRC:   return "crc mismatch";
    case RV9_MOD_ERR_BADABI:   return "incompatible abi";
    case RV9_MOD_ERR_NOMEM:    return "out of memory";
    case RV9_MOD_ERR_IO:       return "storage error";
    case RV9_MOD_ERR_INVAL:    return "invalid module";
    case RV9_MOD_ERR_CONTRACT: return "requires something unsupported";
    case RV9_MOD_ERR_SIGNATURE: return "not signed by a key this machine trusts";
    default:                   return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* The manifest                                                        */
/* ------------------------------------------------------------------ */

/*
 * Walking the list is the same three lines everywhere, and getting the
 * bounds wrong on data read off flash is how a bad module becomes a
 * crash. One walker, used by everything.
 *
 * `fn` sees each entry and stops the walk by returning false.
 */
typedef bool (*tlv_visit_fn)(uint16_t tag, const void *value, uint16_t len,
                             void *ctx);

static void manifest_walk(const void *image, tlv_visit_fn fn, void *ctx)
{
    if (image == NULL) return;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;
    uint32_t off = h->manifest_offset;
    if (off == 0) return;

    /* Every offset in a module is checked against module_len, because the
       header is data and data off a flash partition is not trusted. */
    if (off < h->header_len || off >= h->module_len) return;

    const uint8_t *base = (const uint8_t *)image;

    while (off + sizeof(rv9_mod_tlv_t) <= h->module_len) {
        rv9_mod_tlv_t e;
        memcpy(&e, base + off, sizeof(e));      /* may be unaligned in flash */

        if (RV9_MTAG_NUMBER(e.tag) == RV9_MTAG_END) return;

        uint32_t value_off = off + sizeof(e);
        if (value_off + e.len > h->module_len) return;   /* runs off the end */

        if (!fn(e.tag, base + value_off, e.len, ctx)) return;

        /* Entries are four-byte aligned, so a numeric value is aligned
           when its length is. */
        uint32_t next = value_off + e.len;
        next += (4 - (next % 4)) % 4;
        if (next <= off) return;                /* no progress: malformed */
        off = next;
    }
}

typedef struct {
    uint16_t    want;
    const void *after;      /* skip everything up to and including this */
    bool        armed;      /* past `after` yet */
    const void *found;
    uint16_t    len;
} find_ctx_t;

static bool find_visit(uint16_t tag, const void *value, uint16_t len, void *v)
{
    find_ctx_t *c = (find_ctx_t *)v;

    if (RV9_MTAG_NUMBER(tag) != c->want) return true;

    if (!c->armed) {
        if (value == c->after) c->armed = true;
        return true;
    }

    c->found = value;
    c->len   = len;
    return false;
}

const void *rv9_mod_manifest_find(const void *image, uint16_t tag,
                                  const void *after, uint16_t *out_len)
{
    find_ctx_t c = {
        .want  = RV9_MTAG_NUMBER(tag),
        .after = after,
        .armed = (after == NULL),
    };
    manifest_walk(image, find_visit, &c);

    if (c.found && out_len) *out_len = c.len;
    return c.found;
}

bool rv9_mod_manifest_u32(const void *image, uint16_t tag, uint32_t *out)
{
    uint16_t len = 0;
    const void *v = rv9_mod_manifest_find(image, tag, NULL, &len);
    if (v == NULL || len != sizeof(uint32_t)) return false;

    uint32_t value;
    memcpy(&value, v, sizeof(value));
    if (out) *out = value;
    return true;
}

bool rv9_mod_manifest_u8(const void *image, uint16_t tag, uint8_t *out)
{
    uint16_t len = 0;
    const void *v = rv9_mod_manifest_find(image, tag, NULL, &len);
    if (v == NULL || len != sizeof(uint8_t)) return false;

    if (out) *out = *(const uint8_t *)v;
    return true;
}

/* A manifest longer than this is not one anybody wrote by hand, and one a
   compiler wrote that long is not being scanned from flash on every fork. */
#define MANIFEST_SCAN_MAX 1024

void rv9_mod_note_stack(rv9_mod_entry_t *entry, uint32_t given, uint32_t used)
{
    if (entry == NULL) return;
    rv9_lock_acquire(s_lock);
    entry->stack_given = given;
    if (used > entry->stack_peak) entry->stack_peak = used;
    entry->stack_runs++;
    rv9_lock_release(s_lock);
}

/*
 * Does any module declare this resource under `tag`, and with what meaning?
 *
 * A declaration may carry a unit after a colon --
 * `publishes="/pub0/DISTANCE:mm"` -- so the comparison is on the path and
 * the suffix is an answer rather than part of the question. That is what
 * lets admission say "it watches metres and the publisher writes
 * millimetres" instead of the useless "nothing publishes it".
 *
 * `out_unit` may be NULL. When it is not, it receives the declared unit of
 * the *first* matching declaration, empty if it declared none.
 */

/*
 * A module's header and manifest, wherever it is: its image when it is
 * loaded, else as far as its manifest goes, and a little, from the store
 * or from its file on the path. A copy goes in *buf for the caller to free,
 * with the header in it saying how much was copied, so that a walk bounded
 * by the header runs past nothing. NULL if there is no manifest to read.
 * Called with s_lock held, so no image is freed from under it.
 */
static const void *entry_head(rv9_mod_entry_t *e, uint8_t **buf)
{
    *buf = NULL;
    if (e->image != NULL) return e->image;

    if (e->path[0]) return e->head;
    if (s_store == NULL) return NULL;

    rv9_mod_header_t h;
    if (esp_partition_read(s_store, e->store_offset, &h, sizeof(h)) != ESP_OK) {
        return NULL;
    }
    if (h.manifest_offset == 0 || h.manifest_offset >= h.module_len) return NULL;

    uint32_t n = h.module_len;
    if (n > h.manifest_offset + MANIFEST_SCAN_MAX) n = h.manifest_offset + MANIFEST_SCAN_MAX;
    *buf = rv9_alloc(n);
    if (*buf == NULL) return NULL;
    if (esp_partition_read(s_store, e->store_offset, *buf, n) != ESP_OK) {
        rv9_free(*buf);
        *buf = NULL;
        return NULL;
    }
    ((rv9_mod_header_t *)*buf)->module_len = n;
    return *buf;
}

static bool declares_scan(uint16_t tag, const char *path,
                          char *out_unit, uint32_t cap)
{
    if (path == NULL) return false;
    size_t want = strlen(path);
    bool found = false;

    if (out_unit && cap > 0) out_unit[0] = '\0';

    /* Held throughout, so no image is freed from under the walk. The flash
       reads keep a concurrent link waiting a few milliseconds; admission
       is not a hot path and a link is not a deadline. */
    rv9_lock_acquire(s_lock);

    for (rv9_mod_entry_t *e = s_dir; e != NULL && !found; e = e->next) {
        if (e->type != RV9_MOD_PROGRAM) continue;

        uint8_t *buf = NULL;
        const void *image = entry_head(e, &buf);
        if (image == NULL) continue;

        const void *v = NULL;
        uint16_t len = 0;
        while ((v = rv9_mod_manifest_find(image, tag, v, &len)) != NULL) {
            const char *d = (const char *)v;

            /* The path is everything before a colon, or the whole thing. */
            uint16_t plen = 0;
            while (plen < len && d[plen] != ':') plen++;

            if (plen != want || memcmp(d, path, want) != 0) continue;

            if (out_unit != NULL && cap > 0) {
                uint32_t i = 0;
                for (uint16_t k = plen + 1; k < len && i + 1 < cap; k++) {
                    out_unit[i++] = d[k];
                }
                out_unit[i] = '\0';
            }
            found = true;
            break;
        }

        if (buf) rv9_free(buf);
    }

    rv9_lock_release(s_lock);
    return found;
}

bool rv9_mod_any_declares(uint16_t tag, const char *value)
{
    return declares_scan(tag, value, NULL, 0);
}

bool rv9_mod_declared_meaning(uint16_t tag, const char *path,
                              char *out_unit, uint32_t cap)
{
    return declares_scan(tag, path, out_unit, cap);
}

/*
 * Is there anything in here we have agreed to without understanding?
 *
 * An unknown advisory tag is skipped, which is what makes the format worth
 * having. An unknown mandatory one is a promise this build cannot keep --
 * "never allocate", "this device alone" -- and running the module anyway
 * would be worse than refusing it.
 */
static bool contract_visit(uint16_t tag, const void *value, uint16_t len,
                           void *v)
{
    (void)value; (void)len;

    if ((tag & RV9_MTAG_MANDATORY) == 0)          return true;
    if (RV9_MTAG_NUMBER(tag) <= RV9_MTAG_MAX)     return true;

    *(uint16_t *)v = RV9_MTAG_NUMBER(tag);
    return false;
}

static rv9_mod_err_t manifest_check(const void *image)
{
    uint16_t offender = 0;
    manifest_walk(image, contract_visit, &offender);

    if (offender != 0) {
        ESP_LOGE(TAG, "module requires tag %u, which this build does not "
                      "understand", (unsigned)offender);
        return RV9_MOD_ERR_CONTRACT;
    }
    return RV9_MOD_OK;
}

rv9_mod_err_t rv9_mod_verify(const void *image, size_t avail)
{
    if (image == NULL || avail < RV9_MODULE_HDR_LEN) return RV9_MOD_ERR_INVAL;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;

    if (h->magic != RV9_MODULE_MAGIC)         return RV9_MOD_ERR_BADMAGIC;
    if (h->header_len != RV9_MODULE_HDR_LEN)  return RV9_MOD_ERR_INVAL;
    /* A module built against an older ABI still runs: fields are only ever
       appended. A module built against a newer one cannot. */
    if (h->abi_version > RV9_MODULE_ABI)      return RV9_MOD_ERR_BADABI;
    if (h->module_len < RV9_MODULE_HDR_LEN)   return RV9_MOD_ERR_INVAL;
    if (h->module_len > avail)                return RV9_MOD_ERR_INVAL;
    if (h->entry_offset >= h->module_len)     return RV9_MOD_ERR_INVAL;
    if (h->name_offset >= h->module_len)      return RV9_MOD_ERR_INVAL;

    /* CRC is computed with the crc32 field taken as zero. Rather than
       copying the whole image to patch it, checksum the three spans around
       that field. */
    const uint8_t *p = (const uint8_t *)image;
    const size_t crc_off = offsetof(rv9_mod_header_t, crc32);
    const uint32_t zero = 0;

    uint32_t crc = rv9_crc32(0, p, crc_off);
    crc = rv9_crc32(crc, &zero, sizeof(zero));
    crc = rv9_crc32(crc, p + crc_off + sizeof(uint32_t),
                    h->module_len - crc_off - sizeof(uint32_t));

    if (crc != h->crc32) return RV9_MOD_ERR_BADCRC;

    /* Last, and only once the bytes are known to be the bytes that were
       written: a manifest read out of a corrupt image says nothing. */
    return manifest_check(image);
}

static void dir_append(rv9_mod_entry_t *entry)
{
    entry->next = NULL;
    if (s_dir == NULL) {
        s_dir = entry;
        return;
    }
    rv9_mod_entry_t *tail = s_dir;
    while (tail->next) tail = tail->next;
    tail->next = entry;
}

int rv9_mod_dir_init(void)
{
    if (s_lock == NULL && rv9_lock_create(&s_lock) != RV9_OK) {
        ESP_LOGE(TAG, "could not create directory lock");
        return 0;
    }

    s_store = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_ANY,
                                       RV9_MODULE_PARTITION_LABEL);
    if (s_store == NULL) {
        ESP_LOGE(TAG, "no '%s' partition", RV9_MODULE_PARTITION_LABEL);
        return 0;
    }

    ESP_LOGI(TAG, "module store: %s, %lu KB",
             RV9_MODULE_PARTITION_LABEL, (unsigned long)(s_store->size / 1024));

    /*
     * Map it for execution in place. Not fatal if it fails -- the copy path
     * below still works, which is the point of trying this as an addition
     * rather than a replacement.
     */
    if (esp_partition_mmap(s_store, 0, s_store->size,
                           ESP_PARTITION_MMAP_INST,
                           &s_store_map, &s_store_mmap) == ESP_OK) {
        s_xip = true;
        ESP_LOGI(TAG, "store mapped at %p: modules run in place", s_store_map);
    } else {
        ESP_LOGW(TAG, "store not mappable; modules will be copied to RAM");
    }

    int found = 0;
    uint32_t offset = 0;

    while (offset + RV9_MODULE_HDR_LEN <= s_store->size) {
        rv9_mod_header_t h;
        if (esp_partition_read(s_store, offset, &h, sizeof(h)) != ESP_OK) {
            ESP_LOGE(TAG, "read failed at 0x%lx", (unsigned long)offset);
            break;
        }

        /* Erased flash reads as 0xFF; that is the end of the image. */
        if (h.magic == 0xFFFFFFFFu || h.magic == 0) break;

        if (h.magic != RV9_MODULE_MAGIC) {
            ESP_LOGW(TAG, "bad magic at 0x%lx, stopping scan",
                     (unsigned long)offset);
            break;
        }
        if (h.module_len < RV9_MODULE_HDR_LEN ||
            offset + h.module_len > s_store->size) {
            ESP_LOGW(TAG, "implausible length at 0x%lx", (unsigned long)offset);
            break;
        }

        /* Verify against the real bytes, not just the header. A module that
           fails here is skipped, not fatal -- one bad module should not
           cost us the rest of the store. */
        void *buf = rv9_alloc(h.module_len);
        if (buf == NULL) {
            ESP_LOGE(TAG, "out of memory verifying module at 0x%lx",
                     (unsigned long)offset);
            break;
        }

        rv9_mod_err_t err = RV9_MOD_ERR_IO;
        if (esp_partition_read(s_store, offset, buf, h.module_len) == ESP_OK) {
            err = rv9_mod_verify(buf, h.module_len);
        }

        if (err == RV9_MOD_OK) {
            rv9_mod_entry_t *e = rv9_calloc(1, sizeof(*e));
            if (e == NULL) {
                rv9_free(buf);
                ESP_LOGE(TAG, "out of memory building directory");
                break;
            }
            const char *name = (const char *)buf + h.name_offset;
            strncpy(e->name, name, sizeof(e->name) - 1);
            e->type         = h.type;
            e->revision     = h.revision;
            e->size         = h.module_len;
            e->store_offset = offset;
            dir_append(e);
            found++;
        } else {
            ESP_LOGW(TAG, "module at 0x%lx rejected: %s",
                     (unsigned long)offset, rv9_mod_strerror(err));
        }

        rv9_free(buf);

        /* Modules are 4-byte aligned in the store. */
        offset += (h.module_len + 3u) & ~3u;
    }

    ESP_LOGI(TAG, "%d module%s in directory", found, found == 1 ? "" : "s");
    return found;
}

const rv9_mod_entry_t *rv9_mod_dir_next(const rv9_mod_entry_t *prev)
{
    return prev ? prev->next : s_dir;
}

bool rv9_mod_held(const rv9_mod_entry_t *e)
{
    return e != NULL && e->path[0] == '\0';
}

rv9_mod_err_t rv9_mod_read_at(const rv9_mod_entry_t *e, uint32_t off,
                              void *buf, uint32_t len, uint32_t *got)
{
    if (got) *got = 0;
    if (!rv9_mod_held(e) || buf == NULL) return RV9_MOD_ERR_INVAL;
    if (off >= e->size) return RV9_MOD_OK;
    if (len > e->size - off) len = e->size - off;

    rv9_mod_err_t err = RV9_MOD_OK;
    rv9_lock_acquire(s_lock);
    if (e->resident && e->image != NULL) {
        /* Loaded from bytes: the copy is the only one there is. */
        memcpy(buf, (const uint8_t *)e->image + off, len);
    } else if (s_xip) {
        memcpy(buf, (const uint8_t *)s_store_map + e->store_offset + off, len);
    } else if (s_store == NULL ||
               esp_partition_read(s_store, e->store_offset + off, buf, len) != ESP_OK) {
        err = RV9_MOD_ERR_IO;
    }
    rv9_lock_release(s_lock);
    if (err == RV9_MOD_OK && got) *got = len;
    return err;
}

static const rv9_mod_io_ops_t *s_io_ops;
static const rv9_mod_proc_ops_t *s_proc_ops;

/* ---- the module path ---- */

/*
 * Where to look for a module the store does not have: a list of
 * directories, each tried as <dir><name>.mod, the first found winning.
 * The list is the file /f0/path, read at every miss -- so editing it takes
 * effect at once, and a miss, which is rare, pays for one small read. With
 * no file, the card and then the flash volume.
 *
 * The store is always first. A card can add commands; it cannot replace
 * one the machine was built with, which a person at the board did not ask
 * for. `load` can, by revision, because then somebody did.
 *
 * A directory may be a peer's (/peer/p4/cmd/, design §74), and every board
 * is built every module, so the first <name>.mod found may be one this
 * machine cannot run: listen, built for the P4's FPU, on a C3. Such a one
 * is passed over and the search goes on, so that a path naming two peers
 * finds the copy that runs here. What is fixed about this machine decides
 * it -- the instructions it has and the devices it was built with -- and
 * not what changes by the minute, a device someone holds or a cell nobody
 * publishes yet, which admission says when it is asked. When nothing on
 * the path can run, the first found is still the answer, so that fork
 * refuses it for its reason and `can` says why, rather than "not found".
 */
#define PATH_FILE     "/f0/path"
#define PATH_DEFAULT  "/sd0/cmd/ /f0/cmd/"
#define PATH_MAX_LEN  256
#define PATH_MOD_MAX  (256 * 1024)

/* The whole of a small file, or nothing. */
static int read_small(const char *file, char *buf, uint32_t cap)
{
    int p = s_io_ops->open(file, RV9_MODE_READ);
    if (p < 0) return -1;
    uint32_t got = 0;
    while (got < cap) {
        int n = s_io_ops->read(p, buf + got, cap - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    s_io_ops->close(p);
    return (int)got;
}

/* How much of an image up to `avail` its name and manifest take, the
   manifest's end entry included: what is kept of a module found on the
   path, so that its needs can be read without fetching it again. */
static uint32_t head_extent(const uint8_t *img, uint32_t avail)
{
    const rv9_mod_header_t *h = (const rv9_mod_header_t *)img;
    uint32_t end = h->header_len;
    if (h->name_offset < avail) {
        end = h->name_offset + (uint32_t)strnlen((const char *)img + h->name_offset,
                                                 avail - h->name_offset) + 1;
    }
    uint32_t off = h->manifest_offset;
    if (off == 0 || off < h->header_len || off >= avail) return end < avail ? end : avail;
    while (off + sizeof(rv9_mod_tlv_t) <= avail) {
        rv9_mod_tlv_t t;
        memcpy(&t, img + off, sizeof(t));
        uint32_t next = off + sizeof(t) + t.len;
        if (RV9_MTAG_NUMBER(t.tag) == RV9_MTAG_END || next > avail) {
            off = RV9_MTAG_NUMBER(t.tag) == RV9_MTAG_END ? off + sizeof(t) : avail;
            break;
        }
        off = next + (4 - (next % 4)) % 4;
    }
    if (off > avail) off = avail;
    return off > end ? off : end;
}

/* A copy of that much, with its header saying so, so that a walk bounded by
   the header stops there. */
static uint8_t *head_copy(const uint8_t *img, uint32_t avail)
{
    uint32_t n = head_extent(img, avail);
    uint8_t *c = rv9_alloc(n);
    if (c == NULL) return NULL;
    memcpy(c, img, n);
    ((rv9_mod_header_t *)c)->module_len = n;
    return c;
}

/* The head of the open file `p`, if it is a module called `name`. */
static uint8_t *read_head(int p, const char *name, size_t nl, uint32_t *size)
{
    rv9_mod_header_t h;
    if (s_io_ops->read(p, &h, sizeof(h)) != (int)sizeof(h) ||
        h.magic != RV9_MODULE_MAGIC || h.header_len != sizeof(h) ||
        h.module_len > PATH_MOD_MAX || h.name_offset < sizeof(h) ||
        h.name_offset + nl >= h.module_len) return NULL;

    /* The name and manifest follow the header in every module mkmodule
       writes; one that puts them further off is read up to a bound. */
    uint32_t n = h.name_offset + (uint32_t)nl + 1;
    if (h.manifest_offset >= sizeof(h) && h.manifest_offset < h.module_len &&
        h.manifest_offset + MANIFEST_SCAN_MAX > n) n = h.manifest_offset + MANIFEST_SCAN_MAX;
    if (n > h.module_len) n = h.module_len;
    if (n > 2 * MANIFEST_SCAN_MAX) return NULL;

    uint8_t *buf = rv9_alloc(n);
    if (buf == NULL) return NULL;
    memcpy(buf, &h, sizeof(h));
    uint32_t got = sizeof(h);
    while (got < n) {
        int r = s_io_ops->read(p, buf + got, n - got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    uint8_t *head = NULL;
    if (got == n && memcmp(buf + h.name_offset, name, nl + 1) == 0) {
        *size = h.module_len;
        ((rv9_mod_header_t *)buf)->module_len = n;
        head = head_copy(buf, n);
    }
    rv9_free(buf);
    return head;
}

/* Could it run on this machine, as far as the machine is fixed? If not,
   what it lacks, in `why`. */
static bool runs_here(const uint8_t *head, char *why, size_t cap)
{
    uint32_t isa = 0;
    rv9_sys_limits_t l;
    if (rv9_mod_manifest_u32(head, RV9_MTAG_ISA, &isa) && isa != 0 &&
        s_proc_ops && s_proc_ops->limits && s_proc_ops->limits(&l, sizeof(l)) == 1 &&
        (isa & ~l.isa) != 0) {
        uint32_t lack = isa & ~l.isa;
        snprintf(why, cap, "%s", lack == (RV9_ISA_FPU | RV9_ISA_PIE) ? "the FPU and the vector unit"
                 : (lack & RV9_ISA_FPU) ? "the FPU" : "the vector unit");
        return false;
    }

    static const uint16_t must[] = { RV9_MTAG_DEVICE, RV9_MTAG_EXCLUSIVE };
    for (unsigned k = 0; k < sizeof(must) / sizeof(must[0]) && s_io_ops && s_io_ops->have; k++) {
        const void *v = NULL;
        uint16_t len = 0;
        while ((v = rv9_mod_manifest_find(head, must[k], v, &len)) != NULL) {
            /* The device is the part up to a second slash: /pwm0 of /pwm0/3. */
            const char *d = (const char *)v;
            uint16_t dl = 1;
            while (dl < len && d[dl] != '/' && d[dl] != ':') dl++;
            if (len == 0 || d[0] != '/' || dl >= 16) continue;
            union { char name[16]; rv9_sys_device_t rec; } q;
            memcpy(q.name, d, dl);
            q.name[dl] = '\0';
            if (s_io_ops->have(&q, sizeof(q)) == 0) {
                snprintf(why, cap, "%.*s", (int)dl, d);
                return false;
            }
        }
    }
    return true;
}

static rv9_mod_entry_t *path_entry(const char *name, const char *file,
                                   uint8_t *head, uint32_t size)
{
    rv9_mod_entry_t *e = rv9_calloc(1, sizeof(*e));
    if (e == NULL) {
        rv9_free(head);
        return NULL;
    }
    const rv9_mod_header_t *h = (const rv9_mod_header_t *)head;
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->type     = h->type;
    e->revision = h->revision;
    e->size     = size;
    strncpy(e->path, file, sizeof(e->path) - 1);
    e->head     = head;
    return e;
}

/* The first <dir><name>.mod on the path that is a module of that name and
   can run here, as a directory entry that is not loaded yet; else the first
   that is a module of that name; else NULL. */
static rv9_mod_entry_t *path_find(const char *name)
{
    if (s_io_ops == NULL || strlen(name) >= sizeof(((rv9_mod_entry_t *)0)->name))
        return NULL;

    /* On the heap, not the stack: this runs inside whoever is forking, on
       their stack, with the I/O manager's open below it -- the shell's is
       2.5 KB, and a search made from stack buffers ran it off the end. */
    struct { char list[PATH_MAX_LEN]; char file[64]; char why[40]; } *b = rv9_alloc(sizeof(*b));
    if (b == NULL) return NULL;
    char *list = b->list, *file = b->file;
    rv9_mod_entry_t *found = NULL, *first = NULL;
    int n = read_small(PATH_FILE, list, PATH_MAX_LEN - 1);
    if (n <= 0) {
        strcpy(list, PATH_DEFAULT);
    } else {
        list[n] = '\0';
    }

    for (char *d = list; *d && found == NULL; ) {
        while (*d == ' ' || *d == '\t' || *d == '\n' || *d == '\r') d++;
        if (*d == '\0') break;
        char *end = d;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
        size_t dl = (size_t)(end - d), nl = strlen(name);
        if (dl + nl + 5 <= sizeof(b->file)) {
            memcpy(file, d, dl);
            memcpy(file + dl, name, nl);
            memcpy(file + dl + nl, ".mod", 5);

            int p = s_io_ops->open(file, RV9_MODE_READ);
            if (p >= 0) {
                uint32_t size = 0;
                uint8_t *head = read_head(p, name, nl, &size);
                s_io_ops->close(p);
                if (head == NULL) {
                    ESP_LOGW(TAG, "%s is not the module '%s'", file, name);
                } else if (runs_here(head, b->why, sizeof(b->why))) {
                    found = path_entry(name, file, head, size);
                    break;
                } else {
                    ESP_LOGW(TAG, "%s: passed over: it needs %s", file, b->why);
                    if (first == NULL) {
                        first = path_entry(name, file, head, size);
                    } else {
                        rv9_free(head);
                    }
                }
            }
        }
        d = end;
    }
    rv9_free(b);
    if (found != NULL) {
        if (first != NULL) {
            rv9_free(first->head);
            rv9_free(first);
        }
        return found;
    }
    return first;
}

/*
 * May a module read from a file run here? The policy of rv9_mod_sig_t.
 *
 * /f0/trust holds the keys this machine trusts, one per line:
 *
 *     <name> <key id, 16 hex> <public key, 130 hex>
 *
 * as tools/mkkey.py prints it and `trust add` writes it. With no key there,
 * anything is accepted as it was before signatures, with a warning, which
 * is the bench. With one, a module must be signed by one of them. Read at
 * every check, like /f0/path, so a change is immediate.
 *
 * A module from another machine (/peer/...) gets no bench: it must be
 * signed by a key here whether or not there are others. Nobody at this
 * machine put it there.
 */
#define TRUST_FILE "/f0/trust"
#define TRUST_MAX  2048

static int unhex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_bytes(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = unhex(s[2 * i]), lo = unhex(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static rv9_mod_err_t trust_check(const uint8_t *image, uint32_t len, const char *from)
{
    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;
    bool is_signed = (h->attr & RV9_MOD_ATTR_SIGNED) && len >= h->header_len + sizeof(rv9_mod_sig_t);
    const rv9_mod_sig_t *b = is_signed ? (const rv9_mod_sig_t *)(image + len - sizeof(*b)) : NULL;
    if (b != NULL && b->magic != RV9_MOD_SIG_MAGIC) {
        ESP_LOGE(TAG, "%s: says it is signed and has no signature block", from);
        return RV9_MOD_ERR_SIGNATURE;
    }

    char *text = rv9_alloc(TRUST_MAX + 1);
    if (text == NULL) return RV9_MOD_ERR_NOMEM;
    int n = read_small(TRUST_FILE, text, TRUST_MAX);
    text[n > 0 ? n : 0] = '\0';

    /* Each line: name, key id, key. Find the signer's, and count them. */
    int keys = 0;
    char who[24] = "";
    uint8_t pub[65];
    bool have_pub = false;
    for (char *line = text; *line; ) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char *f[3] = { 0 };
        size_t fl[3] = { 0 };
        char *c = line;
        for (int k = 0; k < 3; k++) {
            while (c < end && (*c == ' ' || *c == '\t' || *c == '\r')) c++;
            f[k] = c;
            while (c < end && *c != ' ' && *c != '\t' && *c != '\r') c++;
            fl[k] = (size_t)(c - f[k]);
        }
        if (fl[0] && f[0][0] != '#' && fl[1] == 16 && fl[2] == 130) {
            keys++;
            uint8_t id[8];
            if (b != NULL && !have_pub && hex_bytes(f[1], id, 8) &&
                memcmp(id, b->key_id, 8) == 0 && hex_bytes(f[2], pub, 65)) {
                have_pub = true;
                size_t wl = fl[0] < sizeof(who) - 1 ? fl[0] : sizeof(who) - 1;
                memcpy(who, f[0], wl);
                who[wl] = '\0';
            }
        }
        line = *end ? end + 1 : end;
    }
    rv9_free(text);

    if (keys == 0 && strncmp(from, "/peer/", 6) == 0) {
        ESP_LOGE(TAG, "%s: from a peer, and %s holds no key to check it with",
                 from, TRUST_FILE);
        return RV9_MOD_ERR_SIGNATURE;
    }
    if (b == NULL) {
        if (keys == 0) {
            ESP_LOGW(TAG, "%s: not signed; accepted, as %s holds no key", from, TRUST_FILE);
            return RV9_MOD_OK;
        }
        ESP_LOGE(TAG, "%s: not signed, and this machine runs only modules "
                      "signed by a key in %s", from, TRUST_FILE);
        return RV9_MOD_ERR_SIGNATURE;
    }
    if (!have_pub) {
        char id[17];
        for (int i = 0; i < 8; i++) {
            id[2 * i] = "0123456789abcdef"[b->key_id[i] >> 4];
            id[2 * i + 1] = "0123456789abcdef"[b->key_id[i] & 15];
        }
        id[16] = '\0';
        if (keys == 0) {
            ESP_LOGW(TAG, "%s: signed by key %s, not checked; accepted, as %s "
                          "holds no key", from, id, TRUST_FILE);
            return RV9_MOD_OK;
        }
        ESP_LOGE(TAG, "%s: signed by key %s, which %s does not hold", from, id,
                 TRUST_FILE);
        return RV9_MOD_ERR_SIGNATURE;
    }

    uint32_t us = 0;
    int v = rv9_sig_verify(image, len, pub, &us);
    if (v != 0) {
        ESP_LOGE(TAG, "%s: %s", from, v == -2 ? "the signature does not match: "
                 "changed since it was signed, or signed by somebody else"
                 : "the signature could not be checked");
        return RV9_MOD_ERR_SIGNATURE;
    }
    ESP_LOGI(TAG, "%s: signed by %s, checked in %lu us", from, who, (unsigned long)us);
    return RV9_MOD_OK;
}

/* A path entry's image, read whole into executable memory and verified. */
static rv9_mod_err_t path_read(rv9_mod_entry_t *e, void **out)
{
    int64_t t0 = rv9_time_us();
    int p = s_io_ops ? s_io_ops->open(e->path, RV9_MODE_READ) : -1;
    if (p < 0) {
        ESP_LOGE(TAG, "%s: gone since it was found (%d)", e->path, p);
        return RV9_MOD_ERR_NOTFOUND;
    }
    uint64_t size = 0;
    int gs = s_io_ops->getstat(p, RV9_GS_SIZE, &size);
    if (gs < 0 || size == 0 || size > PATH_MOD_MAX) {
        ESP_LOGE(TAG, "%s: cannot take %lu bytes (%d)", e->path,
                 (unsigned long)size, gs);
        s_io_ops->close(p);
        return RV9_MOD_ERR_INVAL;
    }
    uint8_t *image = rv9_alloc_exec((size_t)size);
    if (image == NULL) {
        s_io_ops->close(p);
        return RV9_MOD_ERR_NOMEM;
    }
    uint32_t got = 0;
    while (got < size) {
        int n = s_io_ops->read(p, image + got, (uint32_t)size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    s_io_ops->close(p);

    /* The file may have changed since it was found: it is believed now. */
    rv9_mod_err_t err = got == size ? rv9_mod_verify(image, got) : RV9_MOD_ERR_IO;
    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;
    if (err == RV9_MOD_OK && strcmp((const char *)image + h->name_offset, e->name) != 0) {
        err = RV9_MOD_ERR_INVAL;
    }
    if (err == RV9_MOD_OK) err = trust_check(image, got, e->path);
    if (err != RV9_MOD_OK) {
        rv9_free(image);
        ESP_LOGE(TAG, "%s: %s", e->path, rv9_mod_strerror(err));
        return err;
    }
    e->size     = h->module_len;
    e->revision = h->revision;
    ESP_LOGI(TAG, "read '%s' from %s: %lu bytes in %lu us", e->name, e->path,
             (unsigned long)e->size, (unsigned long)(rv9_time_us() - t0));
    *out = image;
    return RV9_MOD_OK;
}

const rv9_mod_entry_t *rv9_mod_find(const char *name)
{
    if (name == NULL) return NULL;

    /* Highest revision wins when names collide -- OS-9 behaviour, and it
       makes replacing a module a matter of loading a newer one. */
    const rv9_mod_entry_t *best = NULL;
    for (rv9_mod_entry_t *e = s_dir; e; e = e->next) {
        if (strcmp(e->name, name) != 0) continue;
        if (best == NULL || e->revision > best->revision) best = e;
    }
    if (best != NULL) return best;

    /* Not on the machine: perhaps on the path. Looked for outside the lock
       -- it is file reads -- and added under it, unless somebody else got
       there first. */
    rv9_mod_entry_t *e = path_find(name);
    if (e == NULL) return NULL;
    rv9_lock_acquire(s_lock);
    for (rv9_mod_entry_t *o = s_dir; o; o = o->next) {
        if (strcmp(o->name, name) == 0) {
            rv9_lock_release(s_lock);
            rv9_free(e->head);
            rv9_free(e);
            return o;
        }
    }
    dir_append(e);
    rv9_lock_release(s_lock);
    ESP_LOGI(TAG, "found '%s' rev %u on the path: %s", e->name, e->revision, e->path);
    return e;
}

/*
 * Add a module that arrived as bytes rather than being found in the store.
 *
 * Its image stays resident: there is nowhere to re-read it from, so it is
 * copied into executable memory once and kept. Replacing a resident module
 * means loading a newer revision, which wins by the same rule as any other
 * name collision.
 */
rv9_mod_err_t rv9_mod_register_image(const void *image, uint32_t len)
{
    rv9_mod_err_t err = rv9_mod_verify(image, len);
    if (err != RV9_MOD_OK) return err;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;

    void *copy = rv9_alloc_exec(h->module_len);
    if (copy == NULL) return RV9_MOD_ERR_NOMEM;
    memcpy(copy, image, h->module_len);
    rv9_isync(copy, h->module_len);

    rv9_mod_entry_t *e = rv9_calloc(1, sizeof(*e));
    if (e == NULL) {
        rv9_free(copy);
        return RV9_MOD_ERR_NOMEM;
    }

    const char *name = (const char *)copy + h->name_offset;
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->type     = h->type;
    e->revision = h->revision;
    e->size     = h->module_len;
    e->image    = copy;
    e->entry    = (rv9_mod_entry_fn)((uint8_t *)rv9_exec_addr(copy) +
                                     h->entry_offset);
    e->resident = true;

    rv9_lock_acquire(s_lock);
    dir_append(e);
    rv9_lock_release(s_lock);

    ESP_LOGI(TAG, "loaded '%s' rev %u, %lu bytes, resident at %p",
             e->name, e->revision, (unsigned long)e->size, e->image);
    return RV9_MOD_OK;
}

rv9_mod_err_t rv9_mod_link(const char *name, rv9_mod_entry_t **out_entry)
{
    if (name == NULL || out_entry == NULL) return RV9_MOD_ERR_INVAL;

    rv9_mod_entry_t *e = (rv9_mod_entry_t *)rv9_mod_find(name);
    if (e == NULL) return RV9_MOD_ERR_NOTFOUND;

    rv9_lock_acquire(s_lock);

    /* Already resident: share it. This is the whole point of reentrant
       module code. */
    if (e->image != NULL) {
        e->link_count++;
        rv9_lock_release(s_lock);
        *out_entry = e;
        return RV9_MOD_OK;
    }

    void *image = NULL;
    bool  in_place = false;

    if (e->path[0]) {
        /* From its file, every time it is not already loaded. The lock is
           let go for the read, and the entry checked again after. */
        rv9_lock_release(s_lock);
        rv9_mod_err_t err = path_read(e, &image);
        if (err != RV9_MOD_OK) return err;
        rv9_lock_acquire(s_lock);
        if (e->image != NULL) {          /* linked meanwhile: share that */
            e->link_count++;
            rv9_lock_release(s_lock);
            rv9_free(image);
            *out_entry = e;
            return RV9_MOD_OK;
        }
        /* The file may have changed since it was found: keep what these
           bytes say they need, not what the first ones did. */
        uint8_t *fresh = head_copy(image, e->size);
        if (fresh != NULL) {
            rv9_free(e->head);
            e->head = fresh;
        }
    } else if (s_xip) {
        /*
         * Where it already lies. Still verified -- the CRC is checked
         * against the mapped bytes, so a corrupt store is caught exactly as
         * before; what is skipped is only the copy.
         *
         * But not for a real-time component, and the reason is the whole of
         * why this is safe to do at all: **a flash erase disables the
         * cache**, so code in mapped flash cannot execute for the duration.
         * That is milliseconds, and it is why RV9_RT_CODE exists and why the
         * GPIO handler is pinned in IRAM -- an edge arriving while the cache
         * is off still has to reach the process waiting for it.
         *
         * A control loop that stalled because something else wrote a file
         * would be a deadline miss caused by an unrelated program, which is
         * the one thing admission is supposed to make impossible. So
         * `class=realtime` pays RAM for its code and keeps determinism;
         * everything else runs in place and pays nothing.
         *
         * The class is read from the mapped bytes before deciding, which is
         * safe: reading is not executing.
         */
        const void *mapped =
            (const uint8_t *)s_store_map + e->store_offset;

        uint8_t cls = RV9_MCLASS_UNSPECIFIED;
        bool realtime = rv9_mod_manifest_u8(mapped, RV9_MTAG_CLASS, &cls) &&
                        cls == RV9_MCLASS_REALTIME;

        if (!realtime && rv9_mod_verify(mapped, e->size) == RV9_MOD_OK) {
            image    = (void *)mapped;
            in_place = true;
        }
    }

    if (!in_place && !e->path[0]) {   /* a path entry's is read already */
        image = rv9_alloc_exec(e->size);
        if (image == NULL) {
            rv9_lock_release(s_lock);
            return RV9_MOD_ERR_NOMEM;
        }

        rv9_mod_err_t err = RV9_MOD_OK;
        if (esp_partition_read(s_store, e->store_offset, image,
                               e->size) != ESP_OK) {
            err = RV9_MOD_ERR_IO;
        } else {
            err = rv9_mod_verify(image, e->size);
        }

        if (err != RV9_MOD_OK) {
            rv9_free(image);
            rv9_lock_release(s_lock);
            return err;
        }
    }

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;
    e->image      = image;
    e->in_place   = in_place;
    e->entry      = (rv9_mod_entry_fn)((uint8_t *)rv9_exec_addr(image) +
                                       h->entry_offset);
    e->link_count = 1;

    /* Only needed when instructions were written through the data path. */
    if (!in_place) rv9_isync(image, e->size);

    rv9_lock_release(s_lock);

    ESP_LOGI(TAG, "linked '%s' %s at %p, entry %p",
             e->name, in_place ? "in place" : "in RAM",
             e->image, (void *)e->entry);

    *out_entry = e;
    return RV9_MOD_OK;
}

rv9_mod_err_t rv9_mod_unlink(rv9_mod_entry_t *entry)
{
    if (entry == NULL) return RV9_MOD_ERR_INVAL;

    rv9_lock_acquire(s_lock);

    if (entry->link_count == 0) {
        rv9_lock_release(s_lock);
        return RV9_MOD_ERR_INVAL;
    }

    if (--entry->link_count == 0 && !entry->resident && !entry->in_place) {
        rv9_free(entry->image);
        entry->image = NULL;
        entry->entry = NULL;
        ESP_LOGI(TAG, "unlinked '%s', image freed", entry->name);
    } else if (entry->link_count == 0 && entry->in_place) {
        /* Nothing to free: it was never anywhere but the store. */
        entry->image    = NULL;
        entry->entry    = NULL;
        entry->in_place = false;
    }

    rv9_lock_release(s_lock);
    return RV9_MOD_OK;
}

/* ---- the environment handed to modules ---- */

static int env_print(const char *s)
{
    if (s == NULL) return -1;

    /* Once the I/O manager is up, print is just a write to stdout -- which
       means every module's output travels the full path/filemgr/driver
       stack rather than shortcutting to the log. */
    if (s_io_ops && s_io_ops->write) {
        size_t n = strlen(s);
        if (s_io_ops->write(RV9_STDOUT, s, (uint32_t)n) >= 0) {
            s_io_ops->write(RV9_STDOUT, "\n", 1);
            return 0;
        }
    }

    ESP_LOGI("module", "%s", s);
    return 0;
}

/*
 * Every service a module asks for is a chance to reschedule it.
 *
 * On a preemptive kernel this costs nothing. On RV-9's own kernel, which
 * switches only when asked, it is what keeps one compute-bound module from
 * owning the machine -- without the module having to know anything about
 * scheduling. A module that computes for a long time while touching
 * nothing is still uninterruptible; that waits for the trap vector.
 */
static uint64_t env_time_ms(void)
{
    rv9_preempt_point();
    return rv9_time_ms();
}

/* No preemption point: this is what a control loop measures itself with,
   and a measurement that can reschedule you measures something else. */
static RV9_RT_CODE uint64_t env_time_us(void) { return rv9_time_us(); }
static void     env_yield(void) { rv9_task_yield(); }
static void     env_sleep_ms(uint32_t ms) { rv9_task_delay_ms(ms); }
static uint32_t env_no_signals(void)
{
    rv9_preempt_point();
    return 0;
}

void rv9_mod_set_io_ops(const rv9_mod_io_ops_t *ops) { s_io_ops = ops; }

void rv9_mod_set_proc_ops(const rv9_mod_proc_ops_t *ops) { s_proc_ops = ops; }

static int env_fork(const char *module, int priority)
{
    return s_proc_ops && s_proc_ops->fork
           ? s_proc_ops->fork(module, priority) : -1;
}

static int env_wait(int pid, int *status, uint32_t timeout_ms)
{
    return s_proc_ops && s_proc_ops->wait
           ? s_proc_ops->wait(pid, status, timeout_ms) : -1;
}

static int env_wait_why(int pid, int *status, int *fault, uint32_t timeout_ms)
{
    if (s_proc_ops && s_proc_ops->wait_why) {
        return s_proc_ops->wait_why(pid, status, fault, timeout_ms);
    }
    /* Falling back loses only the distinction, not the wait. A caller told
       RV9_FAULT_NONE by a system that cannot tell is being given the more
       useful of the two wrong answers: it will report the module's own
       status rather than announcing a crash that did not happen. */
    if (fault) *fault = RV9_FAULT_NONE;
    return env_wait(pid, status, timeout_ms);
}

static int env_signal(int pid, uint32_t signals)
{
    return s_proc_ops && s_proc_ops->signal
           ? s_proc_ops->signal(pid, signals) : -RV9_PE_INVAL;
}

static int env_kill(int pid)
{
    return s_proc_ops && s_proc_ops->kill
           ? s_proc_ops->kill(pid) : -RV9_PE_INVAL;
}

static int env_open(const char *name, uint32_t mode)
{
    return s_io_ops && s_io_ops->open ? s_io_ops->open(name, mode) : -1;
}

static int env_close(int path)
{
    return s_io_ops && s_io_ops->close ? s_io_ops->close(path) : -1;
}

static RV9_RT_CODE int env_read(int path, void *buf, uint32_t len)
{
    rv9_preempt_point();
    return s_io_ops && s_io_ops->read ? s_io_ops->read(path, buf, len) : -1;
}

static RV9_RT_CODE int env_write(int path, const void *buf, uint32_t len)
{
    rv9_preempt_point();
    return s_io_ops && s_io_ops->write ? s_io_ops->write(path, buf, len) : -1;
}

static int env_dup2(int from, int to)
{
    return s_io_ops && s_io_ops->dup2 ? s_io_ops->dup2(from, to) : -1;
}

/*
 * sysinfo. The module directory lives here, so module records are filled
 * directly; process records come back through the process manager's ops,
 * because rv9_module must not depend on rv9_proc.
 */
bool rv9_mod_peek_u32(const char *name, uint16_t tag, uint32_t *out)
{
    rv9_mod_entry_t *e = (rv9_mod_entry_t *)rv9_mod_find(name);
    if (e == NULL) return false;

    rv9_lock_acquire(s_lock);
    uint8_t *copy = NULL;
    const void *image = entry_head(e, &copy);
    bool got = image != NULL && rv9_mod_manifest_u32(image, tag, out);
    rv9_lock_release(s_lock);
    if (copy) rv9_free(copy);
    return got;
}

/* RV9_SYS_NEEDS: a module's manifest, found as a fork would find it. */
static int needs_op(void *buf, uint32_t len)
{
    if (len < sizeof(rv9_sys_needs_t)) return -1;

    char name[sizeof(((rv9_mod_entry_t *)0)->name)];
    size_t n = strnlen((const char *)buf, len);
    if (n == 0 || n >= sizeof(name)) return 0;
    memcpy(name, buf, n + 1);

    rv9_sys_needs_t *out = (rv9_sys_needs_t *)buf;
    rv9_mod_entry_t *e = (rv9_mod_entry_t *)rv9_mod_find(name);
    if (e == NULL || e->type != RV9_MOD_PROGRAM) return 0;

    rv9_lock_acquire(s_lock);
    uint8_t *copy = NULL;
    const rv9_mod_header_t *h = entry_head(e, &copy);

    memset(out, 0, sizeof(*out));
    strncpy(out->from, e->path, sizeof(out->from) - 1);
    out->revision = e->revision;

    uint32_t room = len - (uint32_t)sizeof(*out);
    if (h != NULL && h->manifest_offset != 0 && h->manifest_offset < h->module_len) {
        uint32_t have = h->module_len - h->manifest_offset;
        if (have > MANIFEST_SCAN_MAX) have = MANIFEST_SCAN_MAX;
        if (have > room) have = room & ~3u;     /* whole entries, four-aligned */
        memcpy(out->manifest, (const uint8_t *)h + h->manifest_offset, have);
        out->len = (uint16_t)have;
    }
    rv9_lock_release(s_lock);
    if (copy) rv9_free(copy);
    return 1;
}

static int env_sysinfo(uint32_t what, void *buf, uint32_t len)
{
    if (buf == NULL) return -1;

    switch (what) {
    case RV9_SYS_MEM: {
        /*
         * Fill what the caller asked for, not what we happen to know.
         *
         * This record grows -- the floor added three fields to it. A
         * module built against the shorter version passes the length it
         * knows about, and demanding the current length would turn every
         * addition here into a breaking change for everything already in
         * the store. It gets its prefix, correctly filled.
         */
        if (len < RV9_SYS_MEM_MIN) return -1;

        rv9_sys_mem_t m;
        memset(&m, 0, sizeof(m));

        uint32_t mods = 0;
        for (rv9_mod_entry_t *e = s_dir; e; e = e->next) mods++;

        m.heap_free      = (uint32_t)rv9_heap_free();
        m.heap_low_water = (uint32_t)rv9_heap_low_water();
        m.heap_low_since_up = (uint32_t)rv9_heap_low_since_rebase();
        m.heap_exec_free = (uint32_t)rv9_heap_free_exec();
        m.module_count   = mods;
        m.proc_count     = 0;
        m.heap_floor     = (uint32_t)rv9_heap_floor();
        m.heap_available = (uint32_t)rv9_heap_available();
        m.heap_refusals  = rv9_heap_refusals();
        m.heap_rt_reserve = (uint32_t)rv9_heap_rt_reserve();
        m.heap_largest    = (uint32_t)rv9_heap_largest();
        m.ext_free        = (uint32_t)rv9_heap_large_free();
        m.ext_largest     = (uint32_t)rv9_heap_large_largest();
        m.heap_general    = (uint32_t)rv9_heap_available_for(RV9_MEM_GENERAL);

        if (s_proc_ops && s_proc_ops->procs) {
            int n = s_proc_ops->procs(NULL, 0);
            if (n > 0) m.proc_count = (uint32_t)n;
        }

        memcpy(buf, &m, len < sizeof(m) ? len : sizeof(m));
        return 1;
    }

    case RV9_SYS_DEVICES:
        if (s_io_ops == NULL || s_io_ops->devices == NULL) return -1;
        return s_io_ops->devices(buf, len);

    case RV9_SYS_HAVE:
        if (s_io_ops == NULL || s_io_ops->have == NULL) return -1;
        return s_io_ops->have(buf, len);

    case RV9_SYS_NEEDS:
        return needs_op(buf, len);

    case RV9_SYS_LIMITS:
        if (s_proc_ops == NULL || s_proc_ops->limits == NULL) return -1;
        return s_proc_ops->limits(buf, len);

    case RV9_SYS_BUDGETS:
        if (s_proc_ops == NULL || s_proc_ops->budgets == NULL) return -1;
        return s_proc_ops->budgets(buf, len);

    case RV9_SYS_ADMIT: {
        if (s_proc_ops == NULL || s_proc_ops->rt_load == NULL) return -1;
        return s_proc_ops->rt_load(buf, len);
    }

    case RV9_SYS_CLOCK: {
        if (buf == NULL || len < sizeof(rv9_sys_clock_t)) return -1;
        rv9_sys_clock_t *c = (rv9_sys_clock_t *)buf;

        uint32_t epoch = 0, age = 0;
        bool set = s_clock_read ? s_clock_read(&epoch, &age) : false;

        c->epoch = epoch;
        c->set   = set ? 1u : 0u;
        c->age_s = age;
        return 1;
    }

    case RV9_SYS_LOG: {
        if (buf == NULL || len < sizeof(rv9_sys_log_t)) return -1;
        rv9_sys_log_t *q = (rv9_sys_log_t *)buf;

        uint32_t cap = q->want;
        if (cap > sizeof(q->text)) cap = sizeof(q->text);

        q->held = s_log_held ? s_log_held() : 0;
        q->got  = s_log_read ? s_log_read(q->from, q->text, cap) : 0;
        return (int)q->got;
    }

    case RV9_SYS_MODULES: {
        uint32_t max = len / sizeof(rv9_sys_module_t);
        rv9_sys_module_t *out = (rv9_sys_module_t *)buf;
        uint32_t n = 0;

        for (rv9_mod_entry_t *e = s_dir; e && n < max; e = e->next, n++) {
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].name, e->name, sizeof(out[n].name) - 1);
            out[n].type     = e->type;
            out[n].revision = e->revision;
            out[n].size     = e->size;
            out[n].links    = e->link_count;
        }
        return (int)n;
    }

    case RV9_SYS_STACK_PEAKS: {
        uint32_t max = len / sizeof(rv9_sys_stack_peak_t);
        rv9_sys_stack_peak_t *out = (rv9_sys_stack_peak_t *)buf;
        uint32_t n = 0;

        rv9_lock_acquire(s_lock);
        for (rv9_mod_entry_t *e = s_dir; e && n < max; e = e->next) {
            if (e->stack_runs == 0) continue;
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].name, e->name, sizeof(out[n].name) - 1);
            out[n].given = (uint16_t)(e->stack_given > 0xFFFF ? 0xFFFF : e->stack_given);
            out[n].peak  = (uint16_t)(e->stack_peak  > 0xFFFF ? 0xFFFF : e->stack_peak);
            out[n].runs  = (uint16_t)(e->stack_runs  > 0xFFFF ? 0xFFFF : e->stack_runs);
            n++;
        }
        rv9_lock_release(s_lock);
        return (int)n;
    }

    case RV9_SYS_RT: {
        uint32_t max = len / sizeof(rv9_sys_rt_t);
        rv9_sys_rt_t *out = (rv9_sys_rt_t *)buf;
        uint32_t n = 0;

        /* Walk until the index runs off the end, which the KAL reports
           rather than publishing how many slots it keeps. */
        for (int i = 0; n < max; i++) {
            rv9_rt_stats_t st;
            bool valid = false;

            if (rv9_rt_stats_by_index(i, &st, &valid) != RV9_OK) break;
            if (!valid) continue;

            memset(&out[n], 0, sizeof(out[n]));
            out[n].index           = (uint16_t)i;
            out[n].event_driven    = st.event_driven ? 1 : 0;
            out[n].period_us       = st.period_us;
            out[n].activations     = (uint32_t)st.activations;
            out[n].overruns        = (uint32_t)st.overruns;
            out[n].max_jitter_us   = st.max_jitter_us;
            out[n].max_exec_us     = st.max_exec_us;
            out[n].last_exec_us    = st.last_exec_us;
            out[n].min_interval_us = st.min_interval_us;
            out[n].deadline_us     = st.deadline_us;
            out[n].deadline_misses = (uint32_t)st.deadline_misses;
            out[n].max_response_us = st.max_response_us;
            out[n].floods          = (uint32_t)st.floods;
            out[n].bound_us        = st.bound_us;
            out[n].urgent          = st.urgent ? 1 : 0;
            n++;
        }
        return (int)n;
    }

    case RV9_SYS_STACK:
        if (s_proc_ops && s_proc_ops->stacks) {
            return s_proc_ops->stacks(buf, len);
        }
        return -1;

    case RV9_SYS_PROCS:
        if (s_proc_ops && s_proc_ops->procs) {
            return s_proc_ops->procs(buf, len);
        }
        return -1;

    case RV9_SYS_CLAIM:
        if (s_io_ops && s_io_ops->claims) {
            return s_io_ops->claims(buf, len);
        }
        return -1;

    default:
        return -1;
    }
}

static int env_remove(const char *name)
{
    return s_io_ops && s_io_ops->remove ? s_io_ops->remove(name) : -1;
}

static int env_fork_arg(const char *module, int priority, const char *arg)
{
    return s_proc_ops && s_proc_ops->fork_arg
           ? s_proc_ops->fork_arg(module, priority, arg) : -1;
}

static int env_seek(int path, int32_t offset, int whence)
{
    return s_io_ops && s_io_ops->seek ? s_io_ops->seek(path, offset, whence) : -1;
}

static int env_getstat(int path, uint32_t code, void *arg)
{
    return s_io_ops && s_io_ops->getstat ? s_io_ops->getstat(path, code, arg) : -1;
}

static int env_setstat(int path, uint32_t code, void *arg)
{
    return s_io_ops && s_io_ops->setstat ? s_io_ops->setstat(path, code, arg) : -1;
}

/*
 * Read a module from a path and add it to the directory.
 *
 * Uses the I/O manager, so the module can come from anywhere a path can:
 * the RAM disk today, an SD card when one exists, and -- since it arrived
 * over the network into that file -- effectively from anywhere at all.
 */
rv9_mod_err_t rv9_mod_load_path(const char *path)
{
    if (path == NULL || s_io_ops == NULL) return RV9_MOD_ERR_INVAL;

    int p = s_io_ops->open(path, RV9_MODE_READ);
    if (p < 0) return RV9_MOD_ERR_NOTFOUND;

    uint64_t size = 0;
    if (s_io_ops->getstat(p, RV9_GS_SIZE, &size) < 0 || size == 0 ||
        size > 64 * 1024) {
        s_io_ops->close(p);
        return RV9_MOD_ERR_INVAL;
    }

    uint8_t *buf = (uint8_t *)rv9_alloc((size_t)size);
    if (buf == NULL) {
        s_io_ops->close(p);
        return RV9_MOD_ERR_NOMEM;
    }

    uint32_t got = 0;
    while (got < size) {
        int n = s_io_ops->read(p, buf + got, (uint32_t)size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    s_io_ops->close(p);

    rv9_mod_err_t err = (got == size) ? rv9_mod_verify(buf, got) : RV9_MOD_ERR_IO;
    if (err == RV9_MOD_OK) err = trust_check(buf, got, path);
    if (err == RV9_MOD_OK) err = rv9_mod_register_image(buf, got);
    if (err != RV9_MOD_OK) {
        ESP_LOGE(TAG, "load '%s': %s", path, rv9_mod_strerror(err));
    }

    rv9_free(buf);
    return err;
}

static int env_load(const char *path)
{
    rv9_mod_err_t err = rv9_mod_load_path(path);
    return (err == RV9_MOD_OK) ? 0 : -(int)err;
}

static int env_fork_rt(const char *module, uint32_t period_us,
                       const char *arg)
{
    return s_proc_ops && s_proc_ops->fork_rt
           ? s_proc_ops->fork_rt(module, period_us, arg) : -1;
}

static int env_chain(const char *module)
{
    return s_proc_ops && s_proc_ops->chain ? s_proc_ops->chain(module) : -1;
}

void rv9_mod_env_init(rv9_mod_env_t *env, void *statics,
                      uint32_t statics_size, uint32_t pid)
{
    env->abi_version  = RV9_MODULE_ABI;
    env->statics      = statics;
    env->statics_size = statics_size;
    env->print        = env_print;
    env->time_ms      = env_time_ms;
    env->pid          = pid;
    env->arg          = NULL;
    env->yield        = env_yield;
    env->sleep_ms     = env_sleep_ms;
    env->signals_take = env_no_signals;
    env->open         = env_open;
    env->close        = env_close;
    env->read         = env_read;
    env->write        = env_write;
    env->fork         = env_fork;
    env->wait         = env_wait;
    env->wait_why     = env_wait_why;
    env->sysinfo      = env_sysinfo;
    env->dup2         = env_dup2;
    env->chain        = env_chain;
    env->remove       = env_remove;
    env->fork_arg     = env_fork_arg;
    env->seek         = env_seek;
    env->getstat      = env_getstat;
    env->setstat      = env_setstat;
    env->load         = env_load;

    /* Real-time services are supplied by the process manager, which knows
       whether the caller is entitled to them. A module run outside a
       process gets stubs that refuse. */
    env->rt_declare   = NULL;
    env->rt_wait      = NULL;
    env->rt_stats     = NULL;
    env->fork_rt      = env_fork_rt;
    env->time_us      = env_time_us;
    env->signal       = env_signal;
    env->kill         = env_kill;

    /* An arena is the process manager's to give: one is made at fork. */
    env->heap         = NULL;
    env->heap_size    = 0;
}

rv9_mod_err_t rv9_mod_run(rv9_mod_entry_t *entry, int *out_result)
{
    if (entry == NULL || entry->entry == NULL) return RV9_MOD_ERR_INVAL;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)entry->image;

    void *statics = NULL;
    if (h->static_size > 0) {
        statics = rv9_calloc(1, h->static_size);
        if (statics == NULL) return RV9_MOD_ERR_NOMEM;
    }

    rv9_mod_env_t env;
    rv9_mod_env_init(&env, statics, h->static_size, 0);

    int result = entry->entry(&env);
    if (out_result) *out_result = result;

    rv9_free(statics);
    return RV9_MOD_OK;
}
