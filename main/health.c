/*
 * Keeping well, unattended (design section 82).
 *
 * RV-9 is for machines nobody will be beside when they fail -- a sea drone,
 * a delivery drone, a rover -- so the question this file answers is what a
 * board does about its own failures when there is no one to press reset.
 *
 * A RECORD OF EVERY BOOT. The last sixteen, in NVS so a loss of power
 * keeps them: each one's number, why it started (how the run before it
 * ended), whether it was in safe mode, how long it ran, and where the crash
 * that started it was. `boots` prints them.
 *
 * A COUNT OF BAD ENDS. A panic, a watchdog or a brownout is a bad end; a
 * run proves itself by staying up PROVE_S with its services running, and
 * that clears the count. Three bad ends in a row, none proved, and the next
 * boot is in SAFE MODE: the console, SSH and rshd, the network and peer
 * discovery, the store's modules, and /f0/essential -- nothing a crash
 * could have come from (modules at the top of /f0, /f0/startup, httpd,
 * execd, the self-tests). It says so in the log and in its mDNS
 * announcement, so its peers and whoever is listening know.
 *
 * AND A WAY BACK, BY ITSELF. Nobody may be there to say `safe off`. After
 * an hour in safe mode it tries a normal boot; if that ends badly before it
 * proves itself, it is back in safe mode and waits four times as long,
 * up to a day. Put in safe mode by hand (`safe on`), it stays until told.
 *
 * THE PREVIOUS RUN'S LOG. logring.c keeps the log in memory a reset does
 * not clear; after a bad end this writes it to /f0/blackbox, with what the
 * panic said, so it outlives a loss of power too. `log -p` reads it.
 */
#include "health.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

static const char *TAG = "rv9-health";

#define NVS_NS       "rv9health"
#define NVS_KEY      "state"
#define VERSION      1
#ifndef PROVE_S
#define PROVE_S      300          /* up this long with its services: a good run */
#endif
#define FAILS_SAFE   3
#ifndef WAIT_FIRST_S
#define WAIT_FIRST_S 3600
#endif
#define WAIT_MAX_S   86400
#define SAVE_EVERY_S 3600
#define BOOTS        16
#define BLACKBOX     "/f0/blackbox"

_Static_assert(RV9_RESET_PANIC == ESP_RST_PANIC && RV9_RESET_BROWNOUT == ESP_RST_BROWNOUT &&
               RV9_RESET_TASK_WDT == ESP_RST_TASK_WDT && RV9_RESET_SW == ESP_RST_SW,
               "RV9_RESET_* are ESP-IDF's numbers");

typedef struct {
    uint32_t       version;
    uint32_t       next;          /* the next boot's number */
    uint8_t        fails, safe, trial, manual;
    uint32_t       wait_s;        /* in safe mode: how long before a try */
    uint8_t        n;
    uint8_t        last_bad;      /* RV9_RESET_*: the last bad end's */
    uint8_t        pad[2];
    rv9_sys_boot_t b[BOOTS];      /* oldest first */
} state_t;

static state_t             s;
static rv9_logring_panic_t s_panic;     /* what the last run's panic said */
static uint8_t             s_reason;
static bool                s_proved;
static uint32_t            s_saved_up;

static bool bad_end(uint8_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

static const char *reason_name(uint8_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "the reset pin";
    case ESP_RST_SW:       return "asked to";
    case ESP_RST_PANIC:    return "a panic";
    case ESP_RST_INT_WDT:  return "the interrupt watchdog";
    case ESP_RST_TASK_WDT: return "the task watchdog";
    case ESP_RST_WDT:      return "a watchdog";
    case ESP_RST_BROWNOUT: return "a brownout";
    case ESP_RST_USB:      return "the USB port";
    case ESP_RST_JTAG:     return "JTAG";
    default:               return "unknown";
    }
}

static rv9_sys_boot_t *this_boot(void)
{
    return s.n ? &s.b[s.n - 1] : NULL;
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, &s, sizeof(s)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool rv9_health_boot(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();               /* as drv_net.c does: NVS is unusable as it is */
        e = nvs_flash_init();
    }

    size_t len = sizeof(s);
    nvs_handle_t h;
    bool have = e == ESP_OK && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    if (have) {
        have = nvs_get_blob(h, NVS_KEY, &s, &len) == ESP_OK && len == sizeof(s) &&
               s.version == VERSION;
        nvs_close(h);
    }
    if (!have) {
        memset(&s, 0, sizeof(s));
        s.version = VERSION;
        s.next = 1;
        s.wait_s = WAIT_FIRST_S;
    }

    s_reason = (uint8_t)esp_reset_reason();
    rv9_logring_prev(NULL, &s_panic);
    rv9_sys_boot_t *prev = this_boot();
    bool proved = prev && prev->proved;
    /* How long the run before lasted, from the box: it wrote nothing as
       it died, and its hourly figure is up to an hour stale. */
    if (prev && rv9_logring_prev_alive() > prev->up_s) prev->up_s = rv9_logring_prev_alive();

    if (bad_end(s_reason)) {
        s.last_bad = s_reason;
        s.fails = proved ? 1 : (uint8_t)(s.fails < 255 ? s.fails + 1 : 255);
        if (s.trial && !proved) {
            /* The try out of safe mode failed: back, and wait longer. */
            s.safe = 1;
            s.trial = 0;
            s.wait_s = s.wait_s * 4 > WAIT_MAX_S ? WAIT_MAX_S : s.wait_s * 4;
        }
    }
    /* Not on a try out of safe mode: the count it left with is still 3,
       and the try is judged by how it ends, above, not by that. */
    if (!s.safe && !s.trial && s.fails >= FAILS_SAFE) {
        s.safe = 1;
        s.manual = 0;
        s.wait_s = WAIT_FIRST_S;
    }

    if (s.n == BOOTS) {
        memmove(&s.b[0], &s.b[1], sizeof(s.b[0]) * (BOOTS - 1));
        s.n--;
    }
    rv9_sys_boot_t *b = &s.b[s.n++];
    memset(b, 0, sizeof(*b));
    b->boot = s.next++;
    b->reason = s_reason;
    b->safe = s.safe;
    b->trial = s.trial;
    b->pc = s_panic.valid ? s_panic.pc : 0;
    save();

    if (s.safe) {
        ESP_LOGW(TAG, "SAFE MODE (boot %lu): %u bad end%s in a row, the last %s; "
                      "only the core and /f0/essential run. %s",
                 (unsigned long)b->boot, s.fails, s.fails == 1 ? "" : "s",
                 reason_name(s.last_bad),
                 s.manual ? "Put here by hand: `safe off` to leave."
                          : "It tries a normal boot after a healthy while; `safe off` now.");
    } else if (s.trial) {
        ESP_LOGW(TAG, "boot %lu: trying a normal boot after safe mode", (unsigned long)b->boot);
    }
    rv9_mod_set_health_source(rv9_health_op);
    return s.safe;
}

bool rv9_health_safe(void)
{
    return s.safe != 0;
}

void rv9_health_keep_blackbox(void)
{
    uint32_t len = 0;
    const char *prev = rv9_logring_prev(&len, NULL);
    const rv9_sys_boot_t *b = this_boot();

    if (bad_end(s_reason)) {
        if (s_panic.valid) {
            ESP_LOGW(TAG, "the last run ended in a panic: %s, at pc 0x%08lx (ra 0x%08lx, "
                          "cause %lu, value 0x%08lx); its log is in " BLACKBOX " (`log -p`)",
                     s_panic.reason, (unsigned long)s_panic.pc, (unsigned long)s_panic.ra,
                     (unsigned long)s_panic.cause, (unsigned long)s_panic.tval);
        } else {
            ESP_LOGW(TAG, "the last run ended in %s; its log is in " BLACKBOX " (`log -p`)",
                     reason_name(s_reason));
        }
        rv9_path_t *p = NULL;
        if (rv9_io_open_detached(BLACKBOX, RV9_MODE_WRITE | RV9_MODE_CREATE, &p) == RV9_IO_OK) {
            char head[200];
            int n = snprintf(head, sizeof(head), "-- the run before boot %lu, which ended in %s",
                             (unsigned long)(b ? b->boot : 0), reason_name(s_reason));
            if (s_panic.valid) {
                n += snprintf(head + n, sizeof(head) - n, ": %s at pc 0x%08lx ra 0x%08lx "
                              "cause %lu value 0x%08lx", s_panic.reason,
                              (unsigned long)s_panic.pc, (unsigned long)s_panic.ra,
                              (unsigned long)s_panic.cause, (unsigned long)s_panic.tval);
            }
            n += snprintf(head + n, sizeof(head) - n, " --\n");
            size_t done = 0;
            rv9_io_write_path(p, head, (size_t)n, &done);
            if (prev && len) rv9_io_write_path(p, prev, len, &done);
            else rv9_io_write_path(p, "(nothing kept: it lost power before)\n", 37, &done);
            rv9_io_close_path(p);
        }
    }
    rv9_logring_prev_free();
}

void rv9_health_tick(void)
{
    uint32_t up = (uint32_t)(rv9_time_ms() / 1000);
    rv9_logring_alive(up);
    rv9_sys_boot_t *b = this_boot();
    if (b == NULL) return;

    if (!s_proved && up >= PROVE_S) {
        s_proved = true;
        b->proved = 1;
        b->up_s = up;
        if (!s.safe) {
            if (s.fails || s.trial) {
                ESP_LOGW(TAG, "up %u s with its services: healthy%s", PROVE_S,
                         s.trial ? "; out of safe mode" : "");
            }
            s.fails = 0;
            if (s.trial) {
                s.trial = 0;
                s.wait_s = WAIT_FIRST_S;
            }
        }
        save();
        s_saved_up = up;
    } else if (up - s_saved_up >= SAVE_EVERY_S) {
        b->up_s = up;
        save();
        s_saved_up = up;
    }

    if (s.safe && !s.manual && s_proved && up >= s.wait_s) {
        ESP_LOGW(TAG, "healthy in safe mode for %lu s: trying a normal boot",
                 (unsigned long)up);
        s.safe = 0;
        s.trial = 1;
        b->up_s = up;
        save();
        esp_restart();
    }
}

int rv9_health_op(void *buf, uint32_t len)
{
    if (buf == NULL || len < sizeof(rv9_sys_health_t)) return -1;
    rv9_sys_health_t *o = (rv9_sys_health_t *)buf;
    uint32_t op = o->op;
    uint32_t up = (uint32_t)(rv9_time_ms() / 1000);
    rv9_sys_boot_t *b = this_boot();

    if (op == RV9_HEALTH_SAFE || op == RV9_HEALTH_NORMAL || op == RV9_HEALTH_REBOOT) {
        if (op == RV9_HEALTH_SAFE)   { s.safe = 1; s.manual = 1; s.trial = 0; }
        if (op == RV9_HEALTH_NORMAL) {
            s.safe = 0; s.manual = 0; s.trial = 0; s.fails = 0; s.wait_s = WAIT_FIRST_S;
        }
        if (b) b->up_s = up;
        save();
        ESP_LOGW(TAG, "restarting%s, as asked",
                 op == RV9_HEALTH_SAFE ? " in safe mode" : op == RV9_HEALTH_NORMAL ? " normally" : "");
        rv9_task_delay_ms(200);             /* let the line out */
        esp_restart();
    }
    if (op != RV9_HEALTH_ASK) return -1;

    memset(o, 0, sizeof(*o));
    o->boot = b ? b->boot : 0;
    o->safe = s.safe;
    o->manual = s.manual;
    o->fails = s.fails;
    o->trial = s.trial;
    o->up_s = up;
    if (s.safe && !s.manual) {
        uint32_t at = s.wait_s > PROVE_S ? s.wait_s : PROVE_S;
        o->retry_in_s = up < at ? at - up : 0;
    }
    if (bad_end(s_reason) && s_panic.valid) {
        memcpy(o->panic, s_panic.reason, sizeof(o->panic));
        o->panic[sizeof(o->panic) - 1] = '\0';
        o->panic_pc = s_panic.pc;
        o->panic_ra = s_panic.ra;
        o->panic_cause = s_panic.cause;
        o->panic_tval = s_panic.tval;
    }
    o->n = s.n;
    memcpy(o->boots, s.b, sizeof(s.b[0]) * s.n);
    if (b) o->boots[s.n - 1].up_s = up;
    return 1;
}
