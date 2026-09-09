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

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "rv9-mod";

/*
 * Storage backend.
 *
 * TODO (phase 5): this belongs behind an RBF driver once the I/O manager
 * exists. Modules should come from any block device, not just this one
 * partition. Kept deliberately small so that swap is easy.
 */
#define RV9_MODULE_PARTITION_LABEL "modules"

static const esp_partition_t *s_store;
static rv9_mod_entry_t       *s_dir;
static rv9_mutex_t            s_lock;

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
    default:                   return "unknown error";
    }
}

rv9_mod_err_t rv9_mod_verify(const void *image, size_t avail)
{
    if (image == NULL || avail < RV9_MODULE_HDR_LEN) return RV9_MOD_ERR_INVAL;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;

    if (h->magic != RV9_MODULE_MAGIC)         return RV9_MOD_ERR_BADMAGIC;
    if (h->header_len != RV9_MODULE_HDR_LEN)  return RV9_MOD_ERR_INVAL;
    if (h->abi_version != RV9_MODULE_ABI)     return RV9_MOD_ERR_BADABI;
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

    return (crc == h->crc32) ? RV9_MOD_OK : RV9_MOD_ERR_BADCRC;
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
    if (s_lock == NULL && rv9_mutex_create(&s_lock) != RV9_OK) {
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
    return best;
}

rv9_mod_err_t rv9_mod_link(const char *name, rv9_mod_entry_t **out_entry)
{
    if (name == NULL || out_entry == NULL) return RV9_MOD_ERR_INVAL;

    rv9_mod_entry_t *e = (rv9_mod_entry_t *)rv9_mod_find(name);
    if (e == NULL) return RV9_MOD_ERR_NOTFOUND;

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);

    /* Already resident: share it. This is the whole point of reentrant
       module code. */
    if (e->image != NULL) {
        e->link_count++;
        rv9_mutex_unlock(s_lock);
        *out_entry = e;
        return RV9_MOD_OK;
    }

    void *image = rv9_alloc_exec(e->size);
    if (image == NULL) {
        rv9_mutex_unlock(s_lock);
        return RV9_MOD_ERR_NOMEM;
    }

    rv9_mod_err_t err = RV9_MOD_OK;
    if (esp_partition_read(s_store, e->store_offset, image, e->size) != ESP_OK) {
        err = RV9_MOD_ERR_IO;
    } else {
        err = rv9_mod_verify(image, e->size);
    }

    if (err != RV9_MOD_OK) {
        rv9_free(image);
        rv9_mutex_unlock(s_lock);
        return err;
    }

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;
    e->image      = image;
    e->entry      = (rv9_mod_entry_fn)((uint8_t *)image + h->entry_offset);
    e->link_count = 1;

    /* We just wrote instructions through the data path. */
    rv9_isync();

    rv9_mutex_unlock(s_lock);

    ESP_LOGI(TAG, "linked '%s' at %p, entry %p",
             e->name, e->image, (void *)e->entry);

    *out_entry = e;
    return RV9_MOD_OK;
}

rv9_mod_err_t rv9_mod_unlink(rv9_mod_entry_t *entry)
{
    if (entry == NULL) return RV9_MOD_ERR_INVAL;

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);

    if (entry->link_count == 0) {
        rv9_mutex_unlock(s_lock);
        return RV9_MOD_ERR_INVAL;
    }

    if (--entry->link_count == 0) {
        rv9_free(entry->image);
        entry->image = NULL;
        entry->entry = NULL;
        ESP_LOGI(TAG, "unlinked '%s', image freed", entry->name);
    }

    rv9_mutex_unlock(s_lock);
    return RV9_MOD_OK;
}

/* ---- the environment handed to modules ---- */

static int env_print(const char *s)
{
    if (s == NULL) return -1;
    ESP_LOGI("module", "%s", s);
    return 0;
}

static uint64_t env_time_ms(void) { return rv9_time_ms(); }

rv9_mod_err_t rv9_mod_run(rv9_mod_entry_t *entry, int *out_result)
{
    if (entry == NULL || entry->entry == NULL) return RV9_MOD_ERR_INVAL;

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)entry->image;

    void *statics = NULL;
    if (h->static_size > 0) {
        statics = rv9_calloc(1, h->static_size);
        if (statics == NULL) return RV9_MOD_ERR_NOMEM;
    }

    rv9_mod_env_t env = {
        .abi_version  = RV9_MODULE_ABI,
        .statics      = statics,
        .statics_size = h->static_size,
        .print        = env_print,
        .time_ms      = env_time_ms,
    };

    int result = entry->entry(&env);
    if (out_result) *out_result = result;

    rv9_free(statics);
    return RV9_MOD_OK;
}
