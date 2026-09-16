/*
 * The card, at every boot.
 *
 * Every other subsystem is checked while the machine comes up, and the
 * card should be no different: it is a driver, a file manager and a volume
 * that a program will trust with a file it cannot get back.
 *
 * Two rules shape this. It must skip cleanly on a board with no card --
 * that is the ordinary case, not a failure -- and it must not disturb what
 * is already on the volume: it writes one file of its own, reads it back,
 * and removes it. Nothing here formats anything.
 */
#include "sd_test.h"

#include "rv9/io.h"
#include "rv9/kal.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "sd-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

#define DEV  "/sd0"
#define FILE DEV "/boottest.txt"

static const char LINE[] = "written by the boot test; safe to delete\n";

bool rv9_sd_selftest(void)
{
    s_passed = s_failed = 0;

    /* No card is not a failure: this board is often run without one. */
    int dir = rv9_io_open(DEV, RV9_MODE_READ);
    if (dir < 0) {
        ESP_LOGI(TAG, "no %s on this machine; nothing to check", DEV);
        return true;
    }

    rv9_rbf_space_t space;
    bool have_space = rv9_io_getstat(dir, RV9_RBF_GS_SPACE, &space) == RV9_IO_OK;
    rv9_io_close(dir);

    check(have_space && space.total_sectors > 0 &&
          space.free_sectors <= space.total_sectors,
          "the volume says how much room it has");
    if (have_space) {
        ESP_LOGI(TAG, "  (%lu sectors, %lu free)",
                 (unsigned long)space.total_sectors,
                 (unsigned long)space.free_sectors);
    }

    /* Left over from a boot that was cut short: start from nothing. */
    rv9_io_remove(FILE);

    int p = rv9_io_open(FILE, RV9_MODE_WRITE | RV9_MODE_CREATE);
    check(p >= 0, "a file can be created on the card");
    if (p < 0) goto done;

    size_t done_bytes = 0;
    rv9_io_err_t err = rv9_io_write(p, LINE, sizeof(LINE) - 1, &done_bytes);
    rv9_io_close(p);
    check(err == RV9_IO_OK && done_bytes == sizeof(LINE) - 1,
          "and written to");

    char back[sizeof(LINE)];
    memset(back, 0, sizeof(back));
    p = rv9_io_open(FILE, RV9_MODE_READ);
    check(p >= 0, "and opened again");
    if (p >= 0) {
        done_bytes = 0;
        err = rv9_io_read(p, back, sizeof(LINE) - 1, &done_bytes);
        rv9_io_close(p);
        check(err == RV9_IO_OK && done_bytes == sizeof(LINE) - 1 &&
              memcmp(back, LINE, sizeof(LINE) - 1) == 0,
              "and reads back exactly what was written");
    }

    check(rv9_io_remove(FILE) == RV9_IO_OK, "and can be removed again");

    p = rv9_io_open(FILE, RV9_MODE_READ);
    check(p < 0, "and is gone afterwards");
    if (p >= 0) rv9_io_close(p);

done:
    if (s_failed == 0) {
        ESP_LOGI(TAG, "sd: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "sd: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
