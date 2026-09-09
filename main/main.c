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
#include "kal_selftest.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"

static const char *TAG = "rv9";

#define RV9_VERSION "0.0.2-phase1"

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

/* Placeholder for the process manager. For now it just proves the system
   keeps running and reports heap drift, which is the number that will matter
   most once WiFi arrives. */
static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t beat = 0;

    for (;;) {
        rv9_task_delay_ms(10000);
        ESP_LOGI(TAG, "alive %lus, heap %u free, %u low water",
                 (unsigned long)(rv9_time_ms() / 1000),
                 (unsigned)rv9_heap_free(),
                 (unsigned)rv9_heap_low_water());
        beat++;
    }
}

void app_main(void)
{
    banner();

    if (!rv9_kal_selftest()) {
        ESP_LOGE(TAG, "KAL self-test failed -- not proceeding");
        return;
    }

    ESP_LOGI(TAG, "KAL is sound.");

    rv9_mod_dir_init();
    mdir();
    run_module("hello");

    ESP_LOGI(TAG, "Phase 1 complete.");
    ESP_LOGI(TAG, "next: processes and scheduling (phase 2)");

    rv9_err_t err = rv9_task_create(heartbeat_task, "rv9-heartbeat", 3072,
                                    NULL, RV9_PRIO_LOW, NULL);
    if (err != RV9_OK) {
        ESP_LOGE(TAG, "could not start heartbeat: %s", rv9_strerror(err));
    }
}
