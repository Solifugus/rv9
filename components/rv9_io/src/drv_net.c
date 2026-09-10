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

static const char *TAG = "rv9-net";

typedef struct {
    rv9_net_state_t state;
    uint32_t        ip;
    char            ssid[33];
    bool            wifi_started;
    int             retries;
} net_t;

static net_t *s_net;      /* the event handlers need it; one device only */

#define MAX_RETRIES 5

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg; (void)base; (void)data;

    if (s_net == NULL) return;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net->ip = 0;
        if (s_net->retries < MAX_RETRIES) {
            s_net->retries++;
            s_net->state = RV9_NET_CONNECTING;
            ESP_LOGW(TAG, "disconnected, retry %d", s_net->retries);
            esp_wifi_connect();
        } else {
            s_net->state = RV9_NET_FAILED;
            ESP_LOGE(TAG, "giving up on '%s'", s_net->ssid);
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
    s_net = n;
    dev->drv_state = n;

    ESP_LOGI(TAG, "TCP/IP up; loopback available, radio idle");
    return RV9_IO_OK;
}

/* Association is deferred until someone asks, so a board with no network
   still boots to a shell in the normal time. */
static rv9_io_err_t net_connect(net_t *n, const rv9_net_creds_t *creds)
{
    if (!n->wifi_started) {
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) return RV9_IO_ERR_IO;

        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL);

        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return RV9_IO_ERR_IO;
        n->wifi_started = true;
    } else {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, creds->ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, creds->pass, sizeof(wc.sta.password) - 1);

    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return RV9_IO_ERR_IO;

    strncpy(n->ssid, creds->ssid, sizeof(n->ssid) - 1);
    n->retries = 0;
    n->state = RV9_NET_CONNECTING;

    if (esp_wifi_start() != ESP_OK) return RV9_IO_ERR_IO;

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

    if (code != RV9_NET_GS_STATUS) return RV9_IO_ERR_UNSUPPORTED;

    rv9_net_status_t *st = (rv9_net_status_t *)arg;
    memset(st, 0, sizeof(*st));
    st->state = (uint8_t)n->state;
    st->ip    = n->ip;
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
