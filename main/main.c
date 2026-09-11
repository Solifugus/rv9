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
#include "kal_selftest.h"
#include "kernel_test.h"
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
    ESP_LOGI(TAG, "a modular OS for RISC-V, after OS-9");
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

    rv9_scf_register();
    rv9_rbf_register();
    rv9_nfm_register();
    rv9_drv_uart_register();
    rv9_drv_lcdcon_register();
    rv9_drv_ramdisk_register();
    rv9_drv_flashdisk_register();
    rv9_drv_net_register();

    int n = rv9_io_attach_from_modules();
    ESP_LOGI(TAG, "%d device%s attached from descriptor modules",
             n, n == 1 ? "" : "s");

    devs();

    /* Anything left on the persistent volume becomes a command again. */
    autoload("/f0");

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
    rv9_io_puts(t, "RISC-V, after OS-9\n");
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
#define INV_HOLD_MS   400     /* how long the holder keeps the lock */
#define INV_MEDIUM_MS 600     /* how long the irrelevant hog runs */

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
    rv9_task_create_rt(inv_urgent, "inv-urgent", 4096, NULL, NULL);

    for (int i = 0; i < 400 && s_inv_wait_ms == 0; i++) rv9_task_delay_ms(10);

    rv9_task_delay_ms(INV_MEDIUM_MS);   /* let the hog finish */
    return s_inv_wait_ms;
}

static void inversion_demo(void)
{
    if (rv9_lock_create(&s_inv_lock) != RV9_OK) return;

    ESP_LOGI(TAG, "--- priority inversion ---");

    uint32_t without = inversion_round(false);
    uint32_t with    = inversion_round(true);

    ESP_LOGI(TAG, "urgent waited %lu ms without inheritance, %lu ms with",
             (unsigned long)without, (unsigned long)with);

    if (with < without) {
        ESP_LOGI(TAG, "inheritance cut the wait by %lu ms: the holder ran "
                      "instead of the hog", (unsigned long)(without - with));
    } else {
        ESP_LOGW(TAG, "inheritance made no measurable difference here");
    }

    rv9_lock_set_inheritance(true);
    rv9_lock_destroy(s_inv_lock);
    s_inv_lock = NULL;
}

/* A shell on the network, alongside the one on the cable. */
static void start_rshd(void)
{
    rv9_pid_t pid = 0;
    if (rv9_proc_fork("rshd", RV9_PRIO_LOW, NULL, &pid) == RV9_PROC_OK) {
        ESP_LOGI(TAG, "rshd listening on port 2300 (nc <ip> 2300)");
    } else {
        ESP_LOGW(TAG, "could not start rshd");
    }
}

static void init_shell_loop(void)
{
    start_rshd();

    ESP_LOGI(TAG, "starting shell on /uart0 (log quiet while it runs)");

    for (;;) {
        esp_log_level_set("*", ESP_LOG_WARN);

        rv9_pid_t pid = 0;
        if (rv9_proc_fork("shell", RV9_PRIO_NORMAL, NULL, &pid) != RV9_PROC_OK) {
            esp_log_level_set("*", ESP_LOG_INFO);
            ESP_LOGE(TAG, "could not start shell; giving up");
            return;
        }

        int status = 0;
        rv9_proc_wait(pid, &status, RV9_WAIT_FOREVER);

        esp_log_level_set("*", ESP_LOG_INFO);
        ESP_LOGI(TAG, "shell exited with %d, restarting", status);
        rv9_task_delay_ms(300);
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

    ESP_LOGI(TAG, "process table:");
    ESP_LOGI(TAG, "  %3s %-10s %-8s %4s %4s %4s %6s",
             "pid", "name", "state", "base", "age", "eff", "status");

    for (const rv9_proc_t *p = rv9_proc_next(NULL); p; p = rv9_proc_next(p)) {
        const char *sn = (p->state < 4) ? state_name[p->state] : "?";
        ESP_LOGI(TAG, "  %3u %-10s %-8s %4d %4d %4d %6d",
                 (unsigned)p->pid, p->name, sn,
                 p->base_priority, p->age, p->effective_priority,
                 p->exit_status);
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

    rv9_mod_dir_init();
    mdir();
    io_bringup();
    run_module("hello");

    phase2_demo();
    phase3_demo();
#if RV9_RUN_INVERSION_DEMO
    inversion_demo();
#endif
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
