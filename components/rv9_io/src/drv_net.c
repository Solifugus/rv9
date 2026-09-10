/*
 * net driver -- the link underneath NFM.
 *
 * Brings up the TCP/IP stack at attach time, which is all that loopback
 * needs, and associates with an access point on request. Credentials
 * arrive through setstat, so they are typed by whoever owns the network
 * and never appear in a source file or a descriptor.
 *
 * The C5's radio is dual-band Wi-Fi 6 -- the only ESP32 that does 5 GHz --
 * and nothing here has to care which band it lands on.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "rv9-net";

/* Descriptor options:
 *   opt[0]  band: 0 or 3 = both, 1 = 2.4 GHz only, 2 = 5 GHz only
 */
#define OPT_BAND 0

typedef struct {
    rv9_net_state_t state;
    uint32_t        ip;
    char            ssid[33];
    bool            wifi_started;
    int             retries;
    uint8_t         last_reason;
    uint8_t         band_opt;
} net_t;

static net_t *s_net;      /* the event handlers need it; one device only */

#define MAX_RETRIES 5

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg; (void)base; (void)data;

    if (s_net == NULL) return;

    if (id == WIFI_EVENT_STA_START) {
        /* Only chase an access point if someone has named one. The radio
           also gets started for scanning, and a scan must not trigger an
           association attempt. */
        if (s_net->ssid[0]) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d =
            (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = d ? d->reason : 0;

        s_net->ip = 0;
        s_net->last_reason = reason;

        /* The reason code is the difference between "wrong password" and
           "no such network", which are very different problems. */
        if (s_net->ssid[0] == '\0') {
            s_net->state = RV9_NET_DOWN;
        } else if (s_net->retries < MAX_RETRIES) {
            s_net->retries++;
            s_net->state = RV9_NET_CONNECTING;
            ESP_LOGW(TAG, "disconnected: reason %u, retry %d",
                     (unsigned)reason, s_net->retries);
            esp_wifi_connect();
        } else {
            s_net->state = RV9_NET_FAILED;
            ESP_LOGE(TAG, "giving up on '%s': reason %u",
                     s_net->ssid, (unsigned)reason);
        }
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    if (s_net == NULL) return;

    s_net->ip = e->ip_info.ip.addr;
    s_net->state = RV9_NET_UP;
    s_net->retries = 0;

    /* The supplicant's running commentary was only wanted while we were
       trying to associate. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    ESP_LOGI(TAG, "up: " IPSTR, IP2STR(&e->ip_info.ip));
}

static rv9_io_err_t net_init(rv9_dev_t *dev)
{
    net_t *n = rv9_calloc(1, sizeof(*n));
    if (n == NULL) return RV9_IO_ERR_NOMEM;

    /* Bring up the TCP/IP stack. This alone is enough for loopback, which
       is how NFM gets tested without an access point in the room. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        rv9_free(n);
        return RV9_IO_ERR_IO;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        rv9_free(n);
        return RV9_IO_ERR_IO;
    }

    n->state = RV9_NET_DOWN;
    n->band_opt = (uint8_t)dev->opt[OPT_BAND];
    s_net = n;
    dev->drv_state = n;

    ESP_LOGI(TAG, "TCP/IP up; loopback available, radio idle");
    return RV9_IO_OK;
}

/* Association is deferred until someone asks, so a board with no network
   still boots to a shell in the normal time. */
#define TRY(call)                                                          \
    do {                                                                   \
        esp_err_t _e = (call);                                             \
        if (_e != ESP_OK) {                                                \
            ESP_LOGE(TAG, "%s: %s", #call, esp_err_to_name(_e));            \
            return RV9_IO_ERR_IO;                                          \
        }                                                                  \
    } while (0)

/*
 * Bring the radio up without associating. Scanning needs this as much as
 * connecting does, and a scan must not drag an association along with it.
 */
static rv9_io_err_t net_wifi_up(net_t *n)
{
    if (n->wifi_started) return RV9_IO_OK;
    {
        /*
         * The WiFi driver keeps calibration data in NVS and refuses to
         * initialise without it. Nothing else in RV-9 needs NVS, so this is
         * the only place it comes up -- and it is the kind of dependency
         * that fails silently if you do not check return codes, which is
         * exactly how it failed the first time.
         */
        esp_err_t nvs = nvs_flash_init();
        if (nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "erasing NVS and retrying");
            nvs_flash_erase();
            nvs = nvs_flash_init();
        }
        if (nvs != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nvs));
            return RV9_IO_ERR_IO;
        }

        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        TRY(esp_wifi_init(&cfg));

        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL);

        TRY(esp_wifi_set_mode(WIFI_MODE_STA));

        /*
         * The C5 is the only ESP32 with a 5 GHz radio, and it does not use
         * it unless asked. Left at the default, a 5 GHz-only network simply
         * is not there -- which reports as "no such AP" and sends you
         * looking for a typo in the SSID.
         */
        /*
         * Regulatory domain. The default is the "world safe" profile, under
         * which 5 GHz channels are receive-only: the radio can see an
         * access point on channel 44 and still refuse to associate with it.
         * The failure surfaces as reason 210, "no AP found with compatible
         * security", which sends you hunting through WPA settings for a
         * problem that is not there.
         *
         * Set this to wherever the board actually is.
         */
        esp_err_t country = esp_wifi_set_country_code("US", true);
        if (country != ESP_OK) {
            ESP_LOGW(TAG, "country code: %s", esp_err_to_name(country));
        }

        TRY(esp_wifi_start());

        /* Band selection only takes once the radio is running. */
        wifi_band_mode_t want = WIFI_BAND_MODE_AUTO;
        if (n->band_opt == 1) want = WIFI_BAND_MODE_2G_ONLY;
        else if (n->band_opt == 2) want = WIFI_BAND_MODE_5G_ONLY;

        esp_err_t band = esp_wifi_set_band_mode(want);
        if (band != ESP_OK) {
            ESP_LOGW(TAG, "band mode %d unavailable: %s",
                     (int)want, esp_err_to_name(band));
        } else {
            ESP_LOGI(TAG, "band mode: %s",
                     want == WIFI_BAND_MODE_2G_ONLY ? "2.4 GHz only" :
                     want == WIFI_BAND_MODE_5G_ONLY ? "5 GHz only" : "both");
        }

        n->wifi_started = true;
    }
    return RV9_IO_OK;
}

static rv9_io_err_t net_scan(net_t *n, rv9_net_scan_t *out)
{
    rv9_io_err_t err = net_wifi_up(n);
    if (err != RV9_IO_OK) return err;

    ESP_LOGI(TAG, "scanning...");
    TRY(esp_wifi_scan_start(NULL, true));

    uint16_t found = RV9_NET_MAX_APS;
    static wifi_ap_record_t recs[RV9_NET_MAX_APS];
    TRY(esp_wifi_scan_get_ap_records(&found, recs));

    memset(out, 0, sizeof(*out));
    out->count = (uint8_t)(found > RV9_NET_MAX_APS ? RV9_NET_MAX_APS : found);

    for (int i = 0; i < out->count; i++) {
        strncpy(out->ap[i].ssid, (const char *)recs[i].ssid,
                sizeof(out->ap[i].ssid) - 1);
        out->ap[i].rssi    = recs[i].rssi;
        out->ap[i].channel = recs[i].primary;
        out->ap[i].band    = (recs[i].primary > 14) ? 2 : 1;
        out->ap[i].authmode = (uint8_t)recs[i].authmode;
    }

    ESP_LOGI(TAG, "scan found %u networks", (unsigned)out->count);
    return RV9_IO_OK;
}

static rv9_io_err_t net_connect(net_t *n, const rv9_net_creds_t *creds)
{
    rv9_io_err_t err = net_wifi_up(n);
    if (err != RV9_IO_OK) return err;

    esp_wifi_disconnect();

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, creds->ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, creds->pass, sizeof(wc.sta.password) - 1);

    /*
     * Accept whatever security the access point offers.
     *
     * The defaults are narrower than they look: a modern router in
     * WPA2/WPA3 transition mode is rejected outright, and the failure
     * reports as reason 210 -- "no AP found with compatible security" --
     * which reads like the network is absent rather than merely fussy.
     *
     * authmode OPEN as a *threshold* means "no minimum", not "unencrypted".
     * PMF capable-but-not-required and SAE in both modes covers WPA2, WPA3
     * and the transition mode between them.
     */
    /*
     * memset gave threshold.rssi = 0, which is not "no minimum" -- it is a
     * minimum of 0 dBm, which no real access point reaches. Combined with
     * an authmode threshold this filters out every candidate and reports
     * as 210, "no AP found with compatible security".
     */
    wc.sta.threshold.rssi     = -127;
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable    = true;
    wc.sta.pmf_cfg.required   = false;
    wc.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;

    TRY(esp_wifi_set_config(WIFI_IF_STA, &wc));

    /* The shell quiets the log while it owns the console, which also hides
       the supplicant's account of why it rejected an access point. */
    esp_log_level_set("wifi", ESP_LOG_INFO);
    esp_log_level_set("wifi_init", ESP_LOG_INFO);

    strncpy(n->ssid, creds->ssid, sizeof(n->ssid) - 1);
    n->retries = 0;
    n->state = RV9_NET_CONNECTING;

    TRY(esp_wifi_connect());

    ESP_LOGI(TAG, "associating with '%s'", n->ssid);
    return RV9_IO_OK;
}

static rv9_io_err_t net_setstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    net_t *n = (net_t *)dev->drv_state;
    if (n == NULL) return RV9_IO_ERR_IO;

    switch (code) {
    case RV9_NET_SS_CONNECT:
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        return net_connect(n, (const rv9_net_creds_t *)arg);

    case RV9_NET_SS_DISCONNECT:
        if (n->wifi_started) {
            esp_wifi_disconnect();
            n->state = RV9_NET_DOWN;
            n->ip = 0;
        }
        return RV9_IO_OK;

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static rv9_io_err_t net_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    net_t *n = (net_t *)dev->drv_state;
    if (n == NULL || arg == NULL) return RV9_IO_ERR_IO;

    if (code == RV9_NET_GS_SCAN) return net_scan(n, (rv9_net_scan_t *)arg);
    if (code != RV9_NET_GS_STATUS) return RV9_IO_ERR_UNSUPPORTED;

    rv9_net_status_t *st = (rv9_net_status_t *)arg;
    memset(st, 0, sizeof(*st));
    st->state  = (uint8_t)n->state;
    st->ip     = n->ip;
    st->reason = n->last_reason;
    strncpy(st->ssid, n->ssid, sizeof(st->ssid) - 1);
    return RV9_IO_OK;
}

static const rv9_driver_t net = {
    .name    = "net",
    .init    = net_init,
    .getstat = net_getstat,
    .setstat = net_setstat,
};

rv9_io_err_t rv9_drv_net_register(void)
{
    return rv9_io_register_driver(&net);
}
