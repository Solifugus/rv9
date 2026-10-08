/*
 * What time it is, as opposed to how long we have been up.
 *
 * Everything RV-9 has measured until now has been uptime: rv9_time_ms()
 * counts from reset, the log stamps lines with milliseconds since boot,
 * and a publication's age is a difference between two of those. That is
 * exactly right for a control loop and useless the moment a second
 * machine is involved -- "this reading is 40 ms old" composes across a
 * wire; "this happened at 34,761 ms" does not, because the other end
 * booted at a different instant.
 *
 * So the board asks the network what time it is.
 *
 * THERE IS NO CLOCK ON THIS BOARD
 *
 * No battery, no RTC that survives power. Wall-clock time is therefore
 * *acquired*, not kept: unknown from reset until the network answers, and
 * unknown again after the next reset. Code that reads it must cope with
 * not having it, which is why every reader is told whether it has been
 * set rather than being handed a plausible-looking zero.
 *
 * Unset reads as zero and `synced` reads as false. A wrong time is worse
 * than no time -- a log correlated against a clock that was quietly
 * reporting 1970 sends somebody hunting in the wrong hour.
 *
 * UTC, DELIBERATELY
 *
 * No timezone, no daylight saving, no table of rules that changes when a
 * government decides it should. The board says what the network said.
 * Whatever displays it can subtract; that is a question about where a
 * person is standing, and the machine does not know.
 */
#include "clock.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"

#include "rv9/kal.h"

static const char *TAG = "rv9-clock";

/*
 * A BOARD WITH NO RADIO HAS NOTHING TO ASK
 *
 * Wall-clock time here is acquired over the network, and the ESP32-P4 has no
 * network of its own -- its WiFi lives on a companion chip reached over SDIO
 * (see components/rv9_io/CMakeLists.txt). So on the P4 there is no SNTP to
 * start and the time is simply never known.
 *
 * Which is a state this file already has, and already handles: unset reads as
 * zero and `synced` reads as false, because the time is unknown from reset
 * until something answers. `date` prints that honestly rather than printing
 * 1970. The P4 is the case where nothing ever answers, and nothing above has
 * to learn a new one.
 *
 * When esp_wifi_remote arrives this comes back as it was.
 */
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static bool     s_started;
static bool     s_synced;
static uint64_t s_synced_at_ms;      /* uptime when the last answer arrived */

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced      = true;
    s_synced_at_ms = rv9_time_ms();

    time_t now = 0;
    time(&now);
    ESP_LOGI(TAG, "time set from the network: %lu", (unsigned long)now);
}

void rv9_clock_start(void)
{
    if (s_started) return;

    /*
     * pool.ntp.org, which is the answer for a machine that has no opinion
     * about whose clock to trust. A board doing something that matters
     * would name a server it controls, and this is where that would be
     * said -- one line, in a descriptor, rather than anywhere else.
     */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sync;
    cfg.start   = true;

    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        /* Not fatal, and not silent. Everything that needs elapsed time
           still works; only the things that need a *date* do not. */
        ESP_LOGW(TAG, "no time service: the clock stays unset");
        return;
    }

    s_started = true;
    ESP_LOGI(TAG, "asking the network what time it is");
}

bool rv9_clock_read(uint32_t *out_epoch, uint32_t *out_age_s)
{
    if (!s_synced) {
        if (out_epoch) *out_epoch = 0;
        if (out_age_s) *out_age_s = 0;
        return false;
    }

    time_t now = 0;
    time(&now);

    if (out_epoch) *out_epoch = (uint32_t)now;
    if (out_age_s) {
        uint64_t ms = rv9_time_ms();
        *out_age_s = (uint32_t)((ms - s_synced_at_ms) / 1000u);
    }
    return true;
}


