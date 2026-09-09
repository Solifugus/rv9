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
#include "kal_selftest.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"

static const char *TAG = "rv9";

#define RV9_VERSION "0.0.1-phase0"

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
    ESP_LOGI(TAG, "  heap     %u bytes free", (unsigned)rv9_heap_free());
    ESP_LOGI(TAG, "  kernel   FreeRTOS (KAL backend)");
    ESP_LOGI(TAG, "");
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

    ESP_LOGI(TAG, "KAL is sound. Phase 0 complete.");
    ESP_LOGI(TAG, "next: module format and directory (phase 1)");

    rv9_err_t err = rv9_task_create(heartbeat_task, "rv9-heartbeat", 3072,
                                    NULL, RV9_PRIO_LOW, NULL);
    if (err != RV9_OK) {
        ESP_LOGE(TAG, "could not start heartbeat: %s", rv9_strerror(err));
    }
}
