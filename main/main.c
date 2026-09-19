/*
 * RV-9 boot.
 *
 * Phase 0: come up, prove the KAL works, report what we have to work with.
 *
 * Note there is no freertos include here and there never will be. Everything
 * this file needs from the kernel comes through rv9/kal.h. See
 * docs/design.md §4 and tools/check_layering.sh.
 */
#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/proc.h"
#include "rv9/io.h"
#include "rv9/io_builtin.h"
#include "rv9/ssh_builtin.h"
#include "kal_selftest.h"
#include "logring.h"
#include "kernel_test.h"
#include "module_test.h"
#include "io_test.h"
#include "pub_test.h"
#include "fault_test.h"
#include "proc_test.h"
#include "sched_test.h"
#include "mem_test.h"
#include "sd_test.h"
#include "conformance.h"

#define RV9_RUN_KERNEL_TEST 1
#define RV9_RUN_INVERSION_DEMO 0

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "rv9";

#define RV9_VERSION "0.0.6-phase5"

static void banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    (void)esp_flash_get_size(NULL, &flash_bytes);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "RV-9 %s", RV9_VERSION);
    ESP_LOGI(TAG, "a modular OS for RISC-V");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  core     RISC-V, %d core%s, rev v%d.%d",
             chip.cores, chip.cores == 1 ? "" : "s",
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "  flash    %lu MB", (unsigned long)(flash_bytes / (1024 * 1024)));
    ESP_LOGI(TAG, "  heap     %u bytes free, %u executable",
             (unsigned)rv9_heap_free(), (unsigned)rv9_heap_free_exec());
    ESP_LOGI(TAG, "  kernel   FreeRTOS (KAL backend)");
    ESP_LOGI(TAG, "");
}

/* mdir -- list the module directory. Becomes a loadable utility module of
   its own in phase 4; for now it lives here so we can see the directory. */
static void mdir(void)
{
    static const char *type_name[] = {
        "?", "program", "library", "filemgr", "driver",
        "descriptor", "data", "system",
    };

    ESP_LOGI(TAG, "module directory:");
    ESP_LOGI(TAG, "  %-16s %-11s %4s %6s %5s", "name", "type", "rev", "size", "links");

    int count = 0;
    for (const rv9_mod_entry_t *e = rv9_mod_dir_next(NULL);
         e != NULL;
         e = rv9_mod_dir_next(e)) {
        const char *tn = (e->type < 8) ? type_name[e->type] : "?";
        ESP_LOGI(TAG, "  %-16s %-11s %4u %6lu %5lu",
                 e->name, tn, e->revision,
                 (unsigned long)e->size, (unsigned long)e->link_count);
        count++;
    }
    if (count == 0) {
        ESP_LOGW(TAG, "  (empty -- run tools/flash_modules.sh)");
    }
}

/* Load a module, run it, and drop it again. Phase 2 turns this into fork(). */
static void run_module(const char *name)
{
    rv9_mod_entry_t *mod = NULL;

    rv9_mod_err_t err = rv9_mod_link(name, &mod);
    if (err != RV9_MOD_OK) {
        ESP_LOGE(TAG, "link '%s' failed: %s", name, rv9_mod_strerror(err));
        return;
    }

    /* Take a second link to prove sharing works: one image, two holders. */
    rv9_mod_entry_t *shared = NULL;
    if (rv9_mod_link(name, &shared) == RV9_MOD_OK) {
        ESP_LOGI(TAG, "'%s' link count now %lu, image still %p",
                 name, (unsigned long)mod->link_count, mod->image);
        rv9_mod_unlink(shared);
    }

    int result = 0;
    err = rv9_mod_run(mod, &result);
    if (err != RV9_MOD_OK) {
        ESP_LOGE(TAG, "run '%s' failed: %s", name, rv9_mod_strerror(err));
    } else if (result < 0) {
        ESP_LOGE(TAG, "'%s' rejected its environment, code %d", name, result);
    } else {
        ESP_LOGI(TAG, "'%s' returned %d", name, result);
    }

    /* Run it a second time: statics must be freshly zeroed, or the module
       would have failed its own check. Proves per-instance storage. */
    int again = 0;
    if (rv9_mod_run(mod, &again) == RV9_MOD_OK && again == result) {
        ESP_LOGI(TAG, "'%s' re-ran cleanly, statics were fresh", name);
    } else {
        ESP_LOGE(TAG, "'%s' second run differed (%d vs %d)", name, again, result);
    }

    rv9_mod_unlink(mod);
}

/* devs -- list attached devices, showing the binding each descriptor made. */
/*
 * Where the heap stands, at the boundaries of boot.
 *
 * Idle memory went from 44 KB to 26 KB over an afternoon of changes and
 * nothing said where. A warning rather than information, so that it still
 * shows once the console is quietened for the shell.
 */
static void heap_mark(const char *when)
{
    ESP_LOGW(TAG, "heap %-16s free %6u  available %6u  for programs %6u  "
                  "low %6u", when, (unsigned)rv9_heap_free(),
             (unsigned)rv9_heap_available(),
             (unsigned)rv9_heap_available_for(RV9_MEM_GENERAL),
             (unsigned)rv9_heap_low_water());
}

static void devs(void)
{
    ESP_LOGI(TAG, "devices:");
    ESP_LOGI(TAG, "  %-10s %-8s %-8s %5s", "name", "filemgr", "driver", "open");
    for (const rv9_dev_t *d = rv9_io_dev_next(NULL); d; d = rv9_io_dev_next(d)) {
        ESP_LOGI(TAG, "  %-10s %-8s %-8s %5lu",
                 d->name, d->fmgr->name, d->drv->name,
                 (unsigned long)d->open_count);
    }
}

/*
 * Load whatever programs a volume is carrying.
 *
 * This is what makes a program stick: write it to /f0 once and it is a
 * command on every boot afterwards, with no cable involved. A directory is
 * a file, so finding them is an ordinary read.
 */
static void autoload(const char *dev)
{
    int p = rv9_io_open(dev, RV9_MODE_READ);
    if (p < 0) return;

    rv9_dirent_t ents[8];
    int loaded = 0, tried = 0;

    for (;;) {
        size_t got = 0;
        if (rv9_io_read(p, ents, sizeof(ents), &got) != RV9_IO_OK || got == 0) {
            break;
        }

        int count = (int)(got / sizeof(rv9_dirent_t));
        for (int i = 0; i < count; i++) {
            /* Only files that say they are modules. */
            size_t n = strlen(ents[i].name);
            if (n < 5 || strcmp(ents[i].name + n - 4, ".mod") != 0) continue;

            char path[48];
            snprintf(path, sizeof(path), "%s/%s", dev, ents[i].name);

            tried++;
            if (rv9_mod_load_path(path) == RV9_MOD_OK) loaded++;
        }
        if (count < (int)(sizeof(ents) / sizeof(ents[0]))) break;
    }

    rv9_io_close(p);

    if (tried > 0) {
        ESP_LOGI(TAG, "%s: loaded %d of %d module%s", dev, loaded, tried,
                 tried == 1 ? "" : "s");
    }
}

/* Bring up the I/O system: managers and drivers register, then every
   descriptor module in the store is attached. */
static void io_bringup(void)
{
    if (rv9_io_init() != RV9_IO_OK) {
        ESP_LOGE(TAG, "I/O manager failed to start");
        return;
    }

    /*
     * Check every registration.
     *
     * These used to be called and discarded, so when the driver table
     * filled up the ninth driver failed silently and only turned up later
     * as a descriptor that could not find it. A registration that fails is
     * a device that will not exist; say so now, not three layers away.
     */
    #define REGISTER(call)                                                  \
        do {                                                                \
            rv9_io_err_t _e = (call);                                       \
            if (_e != RV9_IO_OK) {                                          \
                ESP_LOGE(TAG, "%s failed: %s", #call, rv9_io_strerror(_e)); \
            }                                                               \
        } while (0)

    REGISTER(rv9_scf_register());
    REGISTER(rv9_rbf_register());
    REGISTER(rv9_nfm_register());
    REGISTER(rv9_pio_register());
    REGISTER(rv9_pfm_register());
    REGISTER(rv9_pipefm_register());
    REGISTER(rv9_drv_uart_register());
    REGISTER(rv9_drv_lcdcon_register());
    REGISTER(rv9_drv_ramdisk_register());
    REGISTER(rv9_drv_flashdisk_register());
    REGISTER(rv9_drv_sdspi_register());
    REGISTER(rv9_drv_net_register());
    REGISTER(rv9_drv_gpio_register());
    REGISTER(rv9_drv_pwm_register());
    REGISTER(rv9_drv_adc_register());
    REGISTER(rv9_drv_tsens_register());
    REGISTER(rv9_drv_svgwin_register());
    REGISTER(rv9_drv_pubmem_register());
    REGISTER(rv9_drv_pipemem_register());
    REGISTER(rv9_drv_ssh_register());

    #undef REGISTER

    int n = rv9_io_attach_from_modules();
    ESP_LOGI(TAG, "%d device%s attached from descriptor modules",
             n, n == 1 ? "" : "s");

    devs();

    /* Anything left on the persistent volume becomes a command again. */
    autoload("/f0");

    /* And from the card, if there is one: a program written to /sd0 is a
       command on the next boot, the same way /f0 works. A board with no
       card opens nothing and this costs nothing. */
    autoload("/sd0");

    /* Processes with no parent inherit these. */
    /* The terminal is the USB cable: keyboard in, characters out. The
       panel is a second display, not the shell's console. */
    rv9_io_set_system_std("/uart0", "/uart0");
}

/* Write the banner to the panel from kernel context, through the same
   stack a module would use. */
static void term_banner(void)
{
    int t = rv9_io_open("/term", RV9_MODE_WRITE);
    if (t < 0) {
        ESP_LOGE(TAG, "could not open /term: %s",
                 rv9_io_strerror((rv9_io_err_t)(-t)));
        return;
    }

    /* The console is 30 columns wide. Keep lines short or they wrap. */
    rv9_io_puts(t, "RV-9 " RV9_VERSION "\n");
    rv9_io_puts(t, "a modular OS for RISC-V\n");
    rv9_io_puts(t, "\n");

    rv9_io_close(t);
    ESP_LOGI(TAG, "banner written to /term");
}

/*
 * init: run a shell, and keep running one.
 *
 * Two things this fixes, both of which showed up the moment a human used it
 * rather than a test script.
 *
 * The kernel log and the shell share one serial line, so routine INFO
 * messages landed in the middle of the shell's output and mangled its
 * tables. While the shell owns the console it gets it to itself; warnings
 * and errors still come through, because those you want to see even if they
 * arrive mid-line.
 *
 * And exiting the only shell used to strand the board. Now it comes back,
 * the way init has always worked.
 *
 * TODO: better still would be sending the kernel log to /term and leaving
 * the serial line entirely to the shell -- two devices, two purposes. That
 * needs a log path that cannot deadlock against the I/O manager it logs
 * through, so it waits for a phase with time to do it properly.
 */
/*
 * Priority inversion, measured.
 *
 * The classic three actors:
 *
 *   low     an ordinary process that takes a lock and then works
 *   medium  an ordinary process that just burns CPU, and wants nothing
 *   urgent  a real-time task that needs the lock
 *
 * Without inheritance, urgent waits for low, and low waits behind medium --
 * so urgent is delayed by a process it does not share anything with. With
 * inheritance, low runs at urgent's priority until it lets go.
 *
 * The number that matters is how long urgent waited.
 */
static void inversion_demo_body(void);

/*
 * Deliberately short.
 *
 * An earlier version held the lock for 400 ms and hogged for 600. That
 * measures the same thing and creates half a second of CPU monopoly at the
 * top of the priority order -- because the real-time waiter blocking on the
 * mutex makes FreeRTOS lend *its* priority to the task the kernel runs in,
 * which then executes CPU-bound threads at that priority. WiFi and the
 * console starve, and the result looked like a lock bug for some time.
 *
 * A real control loop does not monopolise a CPU for half a second, and
 * neither should a test of one. The effect is proportional, so a tenth of
 * the duration shows it just as clearly.
 */
#define INV_HOLD_MS   40      /* how long the holder keeps the lock */
#define INV_MEDIUM_MS 60      /* how long the irrelevant hog runs */

static rv9_lock_t s_inv_lock;
static volatile bool s_inv_go;
static volatile uint32_t s_inv_wait_ms;

static void inv_low(void *arg)
{
    (void)arg;
    rv9_lock_acquire(s_inv_lock);
    s_inv_go = true;

    /* Work while holding it -- deliberately by spinning, so that making
       progress requires actually being scheduled. */
    uint64_t end = rv9_time_ms() + INV_HOLD_MS;
    while (rv9_time_ms() < end) {
        volatile uint32_t acc = 0;
        for (uint32_t i = 0; i < 500; i++) acc += i;
        rv9_preempt_point();
    }

    rv9_lock_release(s_inv_lock);
    rv9_task_delete(NULL);
}

static void inv_medium(void *arg)
{
    (void)arg;
    uint64_t end = rv9_time_ms() + INV_MEDIUM_MS;
    while (rv9_time_ms() < end) {
        volatile uint32_t acc = 0;
        for (uint32_t i = 0; i < 500; i++) acc += i;
        rv9_preempt_point();
    }
    rv9_task_delete(NULL);
}

static void inv_urgent(void *arg)
{
    (void)arg;

    uint64_t t0 = rv9_time_ms();
    rv9_lock_acquire(s_inv_lock);
    s_inv_wait_ms = (uint32_t)(rv9_time_ms() - t0);
    rv9_lock_release(s_inv_lock);

    rv9_task_delete(NULL);
}

static uint32_t inversion_round(bool inherit)
{
    rv9_lock_set_inheritance(inherit);
    s_inv_go = false;
    s_inv_wait_ms = 0;

    rv9_task_create(inv_low, "inv-low", 8192, NULL, RV9_PRIO_LOW, NULL);

    /* Wait until the lock is actually held before the others start. */
    for (int i = 0; i < 200 && !s_inv_go; i++) rv9_task_delay_ms(5);

    rv9_task_create(inv_medium, "inv-med", 8192, NULL, RV9_PRIO_HIGH, NULL);
    rv9_task_create_rt(inv_urgent, "inv-urgent", 4096, NULL, true, NULL);

    for (int i = 0; i < 200 && s_inv_wait_ms == 0; i++) rv9_task_delay_ms(5);

    rv9_task_delay_ms(INV_MEDIUM_MS);   /* let the hog finish */
    return s_inv_wait_ms;
}

/*
 * Run well after boot, not during it.
 *
 * This demonstration puts a task at the very top of the priority order
 * while it runs. Doing that at 2.8 s -- which is exactly when the radio is
 * associating -- starved the WiFi driver during its most timing-sensitive
 * moment, and the result was stalls and panics that looked like a lock bug
 * and were not one.
 *
 * The measurement is the same whenever it is taken. Taking it once the
 * system is idle costs nothing and tells the truth about the locks rather
 * than about what else was happening at the time.
 */
#define INVERSION_DELAY_MS 15000

static void inversion_task(void *arg)
{
    (void)arg;
    rv9_task_delay_ms(INVERSION_DELAY_MS);
    inversion_demo_body();
    rv9_task_delete(NULL);
}

static void inversion_demo_body(void)
{
    if (rv9_lock_create(&s_inv_lock) != RV9_OK) return;

    /* At warning level so it is visible: the shell quiets the log to WARN
       while it owns the console, and this runs long after the shell has
       started. */
    ESP_LOGW(TAG, "--- priority inversion ---");

    uint32_t without = inversion_round(false);
    uint32_t with    = inversion_round(true);

    ESP_LOGW(TAG, "urgent waited %lu ms without inheritance, %lu ms with",
             (unsigned long)without, (unsigned long)with);

    if (with < without) {
        ESP_LOGW(TAG, "inheritance cut the wait by %lu ms: the holder ran "
                      "instead of the hog", (unsigned long)(without - with));
    } else {
        ESP_LOGW(TAG, "inheritance made no measurable difference here");
    }

    rv9_lock_set_inheritance(true);
    rv9_lock_destroy(s_inv_lock);
    s_inv_lock = NULL;
}

static void inversion_demo(void)
{
    rv9_task_create(inversion_task, "inv-demo", 4096, NULL,
                    RV9_PRIO_LOW, NULL);
}

/*
 * The services init keeps running.
 *
 * The console shell always was restarted when it ended; the two network
 * shells were started once and left to it. On a machine nobody is sitting
 * beside, that is backwards -- the way in over the network is the one that
 * matters most, and it was the one that could die for good. `sshd` did,
 * during testing, over a misreading that has since been fixed. The next
 * such misreading should cost a login attempt, not a trip to the board.
 *
 * So all three are supervised alike: noticed when they end, and started
 * again after a pause that doubles while they keep failing and resets
 * once one has stayed up. A service that fails at once every time costs a
 * log line a minute, not a spin.
 */
typedef struct {
    const char *name;
    int         priority;
    const char *says;           /* logged when it starts, the first time */
    rv9_pid_t   pid;
    uint32_t    backoff_ms;
    uint64_t    next_ms;
    uint64_t    started_ms;
    bool        announced;
} service_t;

#define SERVICE_POLL_MS     250
#define SERVICE_BACKOFF_MS  1000
#define SERVICE_BACKOFF_MAX 60000
#define SERVICE_STAYED_UP   60000  /* this long and a failure is a new one */

static service_t s_services[] = {
    { .name = "rshd",  .priority = RV9_PRIO_LOW,
      .says = "rshd listening on port 2300 (nc <ip> 2300)" },
    { .name = "sshd",  .priority = RV9_PRIO_LOW,
      .says = "sshd listening on port 22 (ssh <user>@<ip>)" },
    { .name = "shell", .priority = RV9_PRIO_NORMAL,
      .says = "shell on /uart0 (log quiet while it runs)" },
};

static void init_shell_loop(void)
{
    const size_t n = sizeof(s_services) / sizeof(s_services[0]);

    for (;;) {
        uint64_t now = rv9_time_ms();

        for (size_t i = 0; i < n; i++) {
            service_t *s = &s_services[i];

            if (s->pid != RV9_PID_NONE) {
                int status = 0;
                rv9_proc_err_t w = rv9_proc_wait(s->pid, &status, 0);
                if (w == RV9_PROC_ERR_TIMEOUT) continue;     /* still up */

                /* Ended, or forgotten -- which is ended too. */
                bool stayed = (now - s->started_ms) >= SERVICE_STAYED_UP;
                s->backoff_ms = stayed ? SERVICE_BACKOFF_MS
                              : (s->backoff_ms == 0) ? SERVICE_BACKOFF_MS
                              : (s->backoff_ms * 2 > SERVICE_BACKOFF_MAX)
                                    ? SERVICE_BACKOFF_MAX
                                    : s->backoff_ms * 2;
                ESP_LOGW(TAG, "%s (pid %u) ended with %d; starting it again "
                              "in %lu ms", s->name, (unsigned)s->pid,
                         (w == RV9_PROC_OK) ? status : 0,
                         (unsigned long)s->backoff_ms);
                s->pid     = RV9_PID_NONE;
                s->next_ms = now + s->backoff_ms;
            }

            if (now < s->next_ms) continue;

            rv9_pid_t pid = 0;
            if (rv9_proc_fork(s->name, s->priority, NULL, &pid) == RV9_PROC_OK) {
                s->pid        = pid;
                s->started_ms = now;
                if (!s->announced) {
                    ESP_LOGI(TAG, "%s", s->says);
                    s->announced = true;
                }
            } else {
                s->backoff_ms = (s->backoff_ms == 0) ? SERVICE_BACKOFF_MS
                              : (s->backoff_ms * 2 > SERVICE_BACKOFF_MAX)
                                    ? SERVICE_BACKOFF_MAX
                                    : s->backoff_ms * 2;
                s->next_ms = now + s->backoff_ms;
                ESP_LOGW(TAG, "could not start %s; trying again in %lu ms",
                         s->name, (unsigned long)s->backoff_ms);
            }
        }

        /* Quiet while the console shell is in use; warnings still show,
           which is where a service restarting belongs. */
        esp_log_level_set("*", ESP_LOG_WARN);

        /* Once, when the services have settled: the figure an operator
           logging in will actually have to work with. */
        static int settled;
        if (++settled == 5000 / SERVICE_POLL_MS) heap_mark("services up");

        rv9_task_delay_ms(SERVICE_POLL_MS);
    }
}

static void phase3_demo(void)
{
    term_banner();

    ESP_LOGI(TAG, "--- module I/O through the stack ---");
    rv9_pid_t pid = 0;
    if (rv9_proc_fork("greet", RV9_PRIO_NORMAL, NULL, &pid) == RV9_PROC_OK) {
        int rc = 0;
        rv9_proc_wait(pid, &rc, 5000);
        if (rc == 0) {
            ESP_LOGI(TAG, "greet finished cleanly");
        } else {
            ESP_LOGE(TAG, "greet failed with %d", rc);
        }
    }

    devs();
}

/* procs -- list the process table. Becomes a loadable utility in phase 4. */
static void procs(void)
{
    static const char *state_name[] = { "?", "active", "waiting", "exited" };

    /* A copy, taken under the process lock: a pointer walk of the live
       table would be walking memory that pruning may free. */
    static rv9_sys_proc_t recs[24];
    int n = rv9_proc_list(recs, 24);
    if (n > 24) n = 24;

    ESP_LOGI(TAG, "process table:");
    ESP_LOGI(TAG, "  %3s %-10s %-8s %4s %4s %6s",
             "pid", "name", "state", "base", "eff", "status");

    for (int i = 0; i < n; i++) {
        const rv9_sys_proc_t *p = &recs[i];
        const char *sn = (p->state < 4) ? state_name[p->state] : "?";
        ESP_LOGI(TAG, "  %3u %-10s %-8s %4d %4d %6d",
                 (unsigned)p->pid, p->name, sn,
                 p->base_priority, p->effective_priority, (int)p->status);
    }
}

/*
 * Run two CPU-bound processes at different priorities.
 *
 * The metric that matters is how much work the LOW priority process has
 * done partway through, while both are still running. Final unit counts
 * cannot show starvation: a starved process starts its wall-clock budget
 * late and then runs unimpeded, so it finishes with a full tally having
 * spent the first half of the test getting nothing at all.
 *
 * Total elapsed time is the second tell -- starved processes run
 * sequentially, so the round takes roughly twice as long.
 */
#define SAMPLE_AT_MS 600

static void contention_round(const char *label, bool aging,
                             uint32_t *out_mid_low, uint32_t *out_elapsed)
{
    rv9_proc_aging_set(aging);
    ESP_LOGI(TAG, "--- %s (aging %s) ---", label, aging ? "on" : "off");

    uint64_t t0 = rv9_time_ms();

    rv9_pid_t hi = 0, lo = 0;
    if (rv9_proc_fork("worker", RV9_PRIO_HIGH, NULL, &hi) != RV9_PROC_OK ||
        rv9_proc_fork("worker", RV9_PRIO_LOW,  NULL, &lo) != RV9_PROC_OK) {
        ESP_LOGE(TAG, "fork failed");
        return;
    }

    /* Both forked from one module image -- two processes, one copy of the
       code, separate static storage. This is what reentrancy buys. */
    const rv9_mod_entry_t *m = rv9_mod_find("worker");
    if (m) {
        ESP_LOGI(TAG, "both processes share image %p, link count %lu",
                 m->image, (unsigned long)m->link_count);
    }

    /* Sample the low-priority process while both are still running. The
       first word of worker's statics is its running unit count. */
    rv9_task_delay_ms(SAMPLE_AT_MS);
    uint32_t mid_low = 0, mid_high = 0;
    const uint32_t *lo_st = (const uint32_t *)rv9_proc_statics(lo);
    const uint32_t *hi_st = (const uint32_t *)rv9_proc_statics(hi);
    if (lo_st) mid_low  = *lo_st;
    if (hi_st) mid_high = *hi_st;

    ESP_LOGI(TAG, "at %d ms: high has done %lu units, low has done %lu",
             SAMPLE_AT_MS, (unsigned long)mid_high, (unsigned long)mid_low);

    int hi_units = 0, lo_units = 0;
    rv9_proc_wait(hi, &hi_units, 10000);
    rv9_proc_wait(lo, &lo_units, 10000);

    uint32_t elapsed = (uint32_t)(rv9_time_ms() - t0);
    ESP_LOGI(TAG, "final: high %d units, low %d units, round took %lu ms",
             hi_units, lo_units, (unsigned long)elapsed);

    if (out_mid_low)  *out_mid_low  = mid_low;
    if (out_elapsed)  *out_elapsed  = elapsed;
}

static void signal_demo(void)
{
    ESP_LOGI(TAG, "--- signals ---");

    rv9_pid_t pid = 0;
    if (rv9_proc_fork("worker", RV9_PRIO_NORMAL, NULL, &pid) != RV9_PROC_OK) {
        ESP_LOGE(TAG, "fork failed");
        return;
    }

    rv9_task_delay_ms(300);
    ESP_LOGI(TAG, "sending RV9_SIG_STOP to pid %u", (unsigned)pid);
    rv9_proc_signal(pid, RV9_SIG_STOP);

    int units = 0;
    rv9_proc_wait(pid, &units, 5000);
    ESP_LOGI(TAG, "pid %u stopped early after %d units "
                  "(a full run is ~4x that)", (unsigned)pid, units);
}

static void phase2_demo(void)
{
    if (rv9_proc_init() != RV9_PROC_OK) {
        ESP_LOGE(TAG, "process manager failed to start");
        return;
    }

    uint32_t starved_mid = 0, starved_ms = 0;
    uint32_t aged_mid = 0, aged_ms = 0;

    /*
     * When the kernel ages, aging cannot be switched off to compare
     * against -- and there is nothing left to demonstrate here, because
     * the comparison has already been made in the kernel's own tests.
     */
    if (rv9_sched_ages()) {
        contention_round("kernel aging", true, &aged_mid, &aged_ms);
        ESP_LOGI(TAG, "--- result ---");
        ESP_LOGI(TAG, "low-priority progress at %d ms: %lu units",
                 SAMPLE_AT_MS, (unsigned long)aged_mid);
        if (aged_mid > 0) {
            ESP_LOGI(TAG, "the kernel's own aging kept the low-priority "
                          "process running");
        } else {
            ESP_LOGE(TAG, "the low-priority process got no CPU");
        }
        signal_demo();
        procs();
        return;
    }

    contention_round("without aging", false, &starved_mid, &starved_ms);
    contention_round("with aging",    true,  &aged_mid,    &aged_ms);

    signal_demo();
    procs();

    ESP_LOGI(TAG, "--- result ---");
    ESP_LOGI(TAG, "low-priority progress at %d ms: %lu units starved, "
                  "%lu units aged", SAMPLE_AT_MS,
             (unsigned long)starved_mid, (unsigned long)aged_mid);
    ESP_LOGI(TAG, "round duration: %lu ms starved, %lu ms aged",
             (unsigned long)starved_ms, (unsigned long)aged_ms);

    if (starved_mid == 0 && aged_mid > 0) {
        ESP_LOGI(TAG, "aging works: without it the low-priority process got "
                      "no CPU at all until the high-priority one finished");
    } else if (aged_mid > starved_mid) {
        ESP_LOGI(TAG, "aging helps, though starvation was not total");
    } else {
        ESP_LOGE(TAG, "aging did NOT help -- policy is not working");
    }
}

/*
 * The system runs here rather than in app_main, because it forks processes
 * and must outrank them. app_main sits at the host kernel's default
 * priority, far below any RV-9 process -- forking from there means the
 * child preempts the parent immediately.
 */
static void rv9_init_task(void *arg)
{
    (void)arg;

    /*
     * The tests run here, not in app_main.
     *
     * With the native kernel behind the KAL, the KAL only means anything
     * inside an RV-9 thread: a delay sleeps the calling thread, task_self
     * names it, and a created task needs the scheduler to be running.
     * app_main is a host task and none of that is true there -- which the
     * self-test reported accurately the first time it was asked from the
     * wrong place.
     */
    /*
     * Start keeping the log before the tests run, so that whatever a
     * failing boot has to say is still readable afterwards over the
     * network -- which is the whole reason for keeping it.
     */
    rv9_logring_start();
    rv9_mod_set_log_source(rv9_logring_read, rv9_logring_held);

    if (!rv9_kal_selftest()) {
        ESP_LOGE(TAG, "KAL self-test failed -- not proceeding");
        return;
    }
    ESP_LOGI(TAG, "KAL is sound.");

    /*
     * The kernel tests build and tear down their own kernel, which is fine
     * when RV-9 is a guest and fatal when the kernel underneath us is the
     * one being torn down.
     */
#if RV9_RUN_KERNEL_TEST && !CONFIG_RV9_KERNEL_NATIVE
    rv9_kernel_selftest();
#endif

    /* The KAL contract, against whichever kernel this build runs on. */
    rv9_conformance_run(rv9_ops_freertos());

    rv9_module_selftest();

    /*
     * From here init is the system: it brings the machine up and keeps its
     * services running, and restarting sshd after something ate the
     * ordinary memory is precisely when it must still be able to allocate.
     * After the KAL's own tests, which exercise the floor as an ordinary
     * caller sees it.
     */
    rv9_mem_class_set(RV9_MEM_SYSTEM);

    rv9_mod_dir_init();
    mdir();
    io_bringup();
    heap_mark("after bringup");

    /* After bringup: the claim table comes up with the I/O manager, and
       testing it before then would test nothing. The same goes for
       publication, which needs /pub0 attached. */
    rv9_io_selftest();
    rv9_pub_selftest();
    heap_mark("after io, pub");

    /* Needs the module store, the claim table and /gpio: it forks real
       modules and judges them by the pin they leave behind. */
    rv9_fault_selftest();
    heap_mark("after fault");

    /* Last of the tests, because it forks the most: what it checks is
       that forking a lot leaves the machine as it found it. */
    rv9_proc_selftest();
    heap_mark("after proc");

    /* Real-time scheduling: several loops at once, each for seconds. */
    rv9_sched_selftest();
    heap_mark("after sched");

    /* Memory reserves and budgets: exhausts ordinary memory on purpose, so
       it runs last, when nothing else is starting. */
    rv9_mem_selftest();
    heap_mark("after mem");

    /* The card, if this board has one: a driver, a file manager and a
       volume a program will trust with something it cannot get back. */
    rv9_sd_selftest();

    run_module("hello");

    phase2_demo();
    phase3_demo();
#if RV9_RUN_INVERSION_DEMO
    inversion_demo();
#endif
    /* Every suite has run, mem-test included, so the low-water mark has
       been driven to the floor on purpose. Watch for local lows from here:
       what matters now is the machine as it serves. */
    rv9_heap_low_water_rebase();

    heap_mark("before services");
    init_shell_loop();

    rv9_task_delete(NULL);
}

void app_main(void)
{
    banner();

    /* Whichever kernel backs the KAL brings itself up and runs init. */
    rv9_err_t err = rv9_kal_start(rv9_init_task, "rv9-init", 8192, NULL,
                                  RV9_PRIO_SYSTEM);
    if (err != RV9_OK) {
        ESP_LOGE(TAG, "could not start init: %s", rv9_strerror(err));
    }
}
