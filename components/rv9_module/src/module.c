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

bool rv9_mod_any_declares(uint16_t tag, const char *value)
{
    if (value == NULL) return false;
    size_t want = strlen(value);
    bool found = false;

    /* Held throughout, so no image is freed from under the walk. The flash
       reads keep a concurrent link waiting a few milliseconds; admission
       is not a hot path and a link is not a deadline. */
    rv9_lock_acquire(s_lock);

    for (rv9_mod_entry_t *e = s_dir; e != NULL && !found; e = e->next) {
        if (e->type != RV9_MOD_PROGRAM) continue;

        const void *image = e->image;
        uint8_t *buf = NULL;

        if (image == NULL) {
            if (s_store == NULL) continue;

            rv9_mod_header_t h;
            if (esp_partition_read(s_store, e->store_offset, &h,
                                   sizeof(h)) != ESP_OK) {
                continue;
            }
            if (h.manifest_offset == 0 || h.manifest_offset >= h.module_len) {
                continue;
            }

            uint32_t n = h.module_len;
            if (n > h.manifest_offset + MANIFEST_SCAN_MAX) {
                n = h.manifest_offset + MANIFEST_SCAN_MAX;
            }
            buf = rv9_alloc(n);
            if (buf == NULL) continue;
            if (esp_partition_read(s_store, e->store_offset, buf, n) != ESP_OK) {
                rv9_free(buf);
                continue;
            }

            /* The walk is bounded by the header's length, so the header in
               the copy says how much was copied. Nothing runs past it. */
            ((rv9_mod_header_t *)buf)->module_len = n;
            image = buf;
        }

        const void *v = NULL;
        uint16_t len = 0;
        while ((v = rv9_mod_manifest_find(image, tag, v, &len)) != NULL) {
            if (len == want && memcmp(v, value, want) == 0) {
                found = true;
                break;
            }
        }

        if (buf) rv9_free(buf);
    }

    rv9_lock_release(s_lock);
    return found;
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
    rv9_isync();

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
    e->entry    = (rv9_mod_entry_fn)((uint8_t *)copy + h->entry_offset);
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

    void *image = rv9_alloc_exec(e->size);
    if (image == NULL) {
        rv9_lock_release(s_lock);
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
        rv9_lock_release(s_lock);
        return err;
    }

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)image;
    e->image      = image;
    e->entry      = (rv9_mod_entry_fn)((uint8_t *)image + h->entry_offset);
    e->link_count = 1;

    /* We just wrote instructions through the data path. */
    rv9_isync();

    rv9_lock_release(s_lock);

    ESP_LOGI(TAG, "linked '%s' at %p, entry %p",
             e->name, e->image, (void *)e->entry);

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

    if (--entry->link_count == 0 && !entry->resident) {
        rv9_free(entry->image);
        entry->image = NULL;
        entry->entry = NULL;
        ESP_LOGI(TAG, "unlinked '%s', image freed", entry->name);
    }

    rv9_lock_release(s_lock);
    return RV9_MOD_OK;
}

/* ---- the environment handed to modules ---- */

static const rv9_mod_io_ops_t *s_io_ops;

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

static const rv9_mod_proc_ops_t *s_proc_ops;

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
        m.heap_exec_free = (uint32_t)rv9_heap_free_exec();
        m.module_count   = mods;
        m.proc_count     = 0;
        m.heap_floor     = (uint32_t)rv9_heap_floor();
        m.heap_available = (uint32_t)rv9_heap_available();
        m.heap_refusals  = rv9_heap_refusals();

        if (s_proc_ops && s_proc_ops->procs) {
            int n = s_proc_ops->procs(NULL, 0);
            if (n > 0) m.proc_count = (uint32_t)n;
        }

        memcpy(buf, &m, len < sizeof(m) ? len : sizeof(m));
        return 1;
    }

    case RV9_SYS_ADMIT: {
        if (s_proc_ops == NULL || s_proc_ops->rt_load == NULL) return -1;
        return s_proc_ops->rt_load(buf, len);
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

    rv9_mod_err_t err = (got == size) ? rv9_mod_register_image(buf, got)
                                      : RV9_MOD_ERR_IO;
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
