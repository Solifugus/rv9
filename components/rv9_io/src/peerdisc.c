/*
 * PEERDISC -- the boards on this network, found as printers are found.
 *
 * Every board announces itself by mDNS (DNS-SD): a host name, <name>.local,
 * and a service, _rv9._tcp, whose TXT record carries its id (what `ident`
 * prints) and the ports of its httpd and execd when it runs them. And every
 * thirty seconds it asks who else is there, and keeps what it hears in a
 * table. /peer looks a name up in /f0/peers first and here after, so
 * `/peer/p4/cmd/` on a module path works with no address typed anywhere,
 * and goes on working when the router hands the P4 another one. A PC sees
 * the same announcements: `ssh you@p4.local`, `avahi-browse -r _rv9._tcp`.
 * Design section 81.
 *
 * THE NAME is /f0/name when there is one, else the chip and the end of its
 * MAC, "c3-5852a8", so that four C3s do not all claim "c3".
 *
 * WHAT IS TRUSTED. Discovery says where a board is, and nothing more: a
 * module from it must still be signed (/f0/trust), and a command for it is
 * run only for a caller in its /f0/callers. One thing is added. The first
 * time a name is heard, the id it came with is written to /f0/known, and a
 * board claiming that name later with another id is refused and said: a
 * stranger cannot take the name "p4" over. A board replaced on purpose is
 * a line to delete from /f0/known.
 *
 * In a host task of its own below RV-9, as a signature check is, since an
 * mDNS query waits on the network for a second and a half.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "rv9-peers";

#define NAME_FILE    "/f0/name"
#define KNOWN_FILE   "/f0/known"
#define SERVE_FILE   "/f0/serve"
#define CALLERS_FILE "/f0/callers"
#define RUN_PORT     2301
#define SLOTS        16
#define REFRESH_MS   30000
#define FORGET_MS    150000       /* five refreshes unheard: gone */
#define ASK_MS       1500         /* how long a query listens */
#define ASK_AGAIN_MS 10000        /* a name not heard is asked about this often */
#define KNOWN_MAX    1024
#define TASK_STACK   4096

typedef struct {
    char     name[16];
    uint8_t  ip[4];
    uint16_t http, run;
    uint8_t  id[16];
    uint64_t heard_ms;
    uint8_t  kind;                /* RV9_PEER_HEARD or _REFUSED */
    uint8_t  flags;               /* RV9_PEER_SAFE */
} peer_t;

static peer_t       s_peers[SLOTS];
static rv9_lock_t   s_lock;
static char         s_name[16];
static uint8_t      s_id[16];
static uint16_t     s_http, s_run;
static volatile bool     s_asked;          /* somebody wants a query now */
static volatile bool     s_answered;
static uint64_t     s_last_ask;
static bool         s_started;
static bool         s_safe;                 /* this board is in safe mode */

void rv9_peers_set_safe(bool safe) { s_safe = safe; }

/* ---- small files on /f0 ---- */

static int read_small(const char *file, char *buf, uint32_t cap)
{
    rv9_path_t *p = NULL;
    if (rv9_io_open_detached(file, RV9_MODE_READ, &p) != RV9_IO_OK) return -1;
    uint32_t got = 0;
    size_t n = 0;
    while (got < cap - 1 && rv9_io_read_path(p, buf + got, cap - 1 - got, &n) == RV9_IO_OK && n > 0) {
        got += (uint32_t)n;
    }
    rv9_io_close_path(p);
    buf[got] = '\0';
    return (int)got;
}

static bool exists(const char *file)
{
    rv9_path_t *p = NULL;
    if (rv9_io_open_detached(file, RV9_MODE_READ, &p) != RV9_IO_OK) return false;
    rv9_io_close_path(p);
    return true;
}

static void to_hex(char *out, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = "0123456789abcdef"[b[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool from_hex(uint8_t *out, const char *s, size_t n)
{
    for (size_t i = 0; i < 2 * n; i++) {
        char c = s[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        if (i & 1) out[i / 2] |= (uint8_t)v;
        else       out[i / 2] = (uint8_t)(v << 4);
    }
    return true;
}

/* A name as mDNS will carry it: lower case, letters, digits and '-'. */
static bool good_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= sizeof(s_name)) return false;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
    }
    return true;
}

static void pick_name(void)
{
    char buf[32];
    if (read_small(NAME_FILE, buf, sizeof(buf)) > 0) {
        char *e = buf;
        while (*e && *e != '\n' && *e != '\r' && *e != ' ') e++;
        *e = '\0';
        if (good_name(buf)) {
            strcpy(s_name, buf);
            return;
        }
        ESP_LOGW(TAG, NAME_FILE " says '%s': a name is up to 15 of a-z, 0-9 and '-'", buf);
    }
    /* "esp32c3" -> "c3", and the end of the MAC */
    const char *chip = CONFIG_IDF_TARGET;
    if (strncmp(chip, "esp32", 5) == 0 && chip[5]) chip += 5;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), "%.6s-%02x%02x%02x", chip, mac[3], mac[4], mac[5]);
}

/*
 * The id a name was first heard with, from /f0/known; and a name heard for
 * the first time is written there. True when `id` may be believed.
 */
static bool known_ok(const char *name, const uint8_t id[16])
{
    char *buf = rv9_alloc(KNOWN_MAX);
    if (buf == NULL) return false;
    int n = read_small(KNOWN_FILE, buf, KNOWN_MAX - 64);
    if (n < 0) n = 0;
    char want[33];
    to_hex(want, id, 16);
    size_t nl = strlen(name);

    for (const char *line = buf; *line; ) {
        const char *end = strchr(line, '\n');
        if (strncmp(line, name, nl) == 0 && line[nl] == ' ') {
            bool same = strncasecmp(line + nl + 1, want, 32) == 0;
            rv9_free(buf);
            return same;
        }
        if (!end) break;
        line = end + 1;
    }

    /* First heard: remembered. The file is written again whole with the
       line added -- a detached path has no seek to append with, and it is
       a few dozen bytes a board, once. */
    if (n > 0 && buf[n - 1] != '\n') buf[n++] = '\n';
    n += snprintf(buf + n, 64, "%s %s\n", name, want);
    rv9_path_t *p = NULL;
    if (rv9_io_open_detached(KNOWN_FILE, RV9_MODE_WRITE | RV9_MODE_CREATE, &p) == RV9_IO_OK) {
        size_t done = 0;
        rv9_io_write_path(p, buf, (size_t)n, &done);
        rv9_io_close_path(p);
        ESP_LOGW(TAG, "%s heard for the first time, as %.8s...; remembered in " KNOWN_FILE,
                 name, want);
    }
    rv9_free(buf);
    return true;
}

/* ---- announcing ---- */

static void ports_now(void)
{
    char buf[12];
    s_http = 0;
    if (read_small(SERVE_FILE, buf, sizeof(buf)) >= 0) {
        int port = atoi(buf);
        s_http = (uint16_t)(port > 0 && port < 65536 ? port : 8080);
    }
    s_run = exists(CALLERS_FILE) ? RUN_PORT : 0;
}

static void txt_now(void)
{
    char id[33], http[8], run[8];
    to_hex(id, s_id, 16);
    snprintf(http, sizeof(http), "%u", s_http);
    snprintf(run, sizeof(run), "%u", s_run);
    mdns_service_txt_item_set("_rv9", "_tcp", "id", id);
    mdns_service_txt_item_set("_rv9", "_tcp", "http", http);
    mdns_service_txt_item_set("_rv9", "_tcp", "run", run);
    /* So its peers, and whoever is listening, know it is in safe mode. */
    mdns_service_txt_item_set("_rv9", "_tcp", "mode", s_safe ? "safe" : "normal");
}

static bool announce(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS would not start; peers only from /f0/peers");
        return false;
    }
    mdns_hostname_set(s_name);
    char inst[32];
    snprintf(inst, sizeof(inst), "RV-9 %s", s_name);
    mdns_instance_name_set(inst);
    ports_now();
    if (mdns_service_add(NULL, "_rv9", "_tcp", RUN_PORT, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "could not announce _rv9._tcp");
        return false;
    }
    if (rv9_ident_get(NULL, s_id) == false) memset(s_id, 0, sizeof(s_id));
    txt_now();
    ESP_LOGW(TAG, "this board is %s (%s.local)", s_name, s_name);
    return true;
}

/* ---- listening ---- */

static const char *txt(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) return r->txt[i].value;
    }
    return NULL;
}

static void heard(const mdns_result_t *r)
{
    if (r->hostname == NULL || !good_name(r->hostname) || strcmp(r->hostname, s_name) == 0) return;
    uint8_t ip[4] = { 0 };
    bool have_ip = false;
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            uint32_t v = a->addr.u_addr.ip4.addr;          /* network order */
            memcpy(ip, &v, 4);
            have_ip = true;
            break;
        }
    }
    if (!have_ip) return;

    uint8_t id[16] = { 0 };
    const char *ids = txt(r, "id");
    bool has_id = ids && strlen(ids) == 32 && from_hex(id, ids, 16);
    const char *h = txt(r, "http"), *u = txt(r, "run"), *mode = txt(r, "mode");
    uint8_t kind = (!has_id || known_ok(r->hostname, id)) ? RV9_PEER_HEARD : RV9_PEER_REFUSED;

    rv9_lock_acquire(s_lock);
    peer_t *slot = NULL, *oldest = &s_peers[0];
    for (int i = 0; i < SLOTS; i++) {
        if (strcmp(s_peers[i].name, r->hostname) == 0) { slot = &s_peers[i]; break; }
        if (s_peers[i].heard_ms < oldest->heard_ms) oldest = &s_peers[i];
    }
    if (slot == NULL) slot = oldest;
    bool news = slot->kind != kind || strcmp(slot->name, r->hostname) != 0 ||
                memcmp(slot->ip, ip, 4) != 0;
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, r->hostname, sizeof(slot->name) - 1);
    memcpy(slot->ip, ip, 4);
    memcpy(slot->id, id, 16);
    slot->http = (uint16_t)(h ? atoi(h) : 0);
    slot->run = (uint16_t)(u ? atoi(u) : 0);
    slot->heard_ms = rv9_time_ms();
    slot->kind = kind;
    slot->flags = (mode && strcmp(mode, "safe") == 0) ? RV9_PEER_SAFE : 0;
    rv9_lock_release(s_lock);

    if (news && kind == RV9_PEER_REFUSED) {
        ESP_LOGW(TAG, "%s at %u.%u.%u.%u is not the %s first heard (another id): not used; "
                      "if it was replaced, delete its line from " KNOWN_FILE,
                 r->hostname, ip[0], ip[1], ip[2], ip[3], r->hostname);
    } else if (news) {
        ESP_LOGI(TAG, "heard %s at %u.%u.%u.%u", r->hostname, ip[0], ip[1], ip[2], ip[3]);
    }
}

static void ask(void)
{
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_rv9", "_tcp", ASK_MS, SLOTS, &results) == ESP_OK) {
        for (const mdns_result_t *r = results; r; r = r->next) heard(r);
    }
    if (results) mdns_query_results_free(results);
    s_last_ask = rv9_time_ms();
    s_answered = true;
}

static void discovery(void *arg)
{
    (void)arg;
    if (!announce()) { rv9_task_delete(NULL); return; }
    uint64_t next = rv9_time_ms() + 3000;          /* the first, once WiFi is up */
    for (;;) {
        uint64_t now = rv9_time_ms();
        if (s_asked || now >= next) {
            s_asked = false;
            ports_now();
            txt_now();
            ask();
            next = rv9_time_ms() + REFRESH_MS;
        }
        rv9_task_delay_ms(100);
    }
}

/* ---- for /peer and sysinfo ---- */

bool rv9_peer_heard(const char *name, uint8_t ip[4], uint16_t *http)
{
    if (!s_started) return false;
    for (int pass = 0; pass < 2; pass++) {
        rv9_lock_acquire(s_lock);
        uint64_t now = rv9_time_ms();
        for (int i = 0; i < SLOTS; i++) {
            peer_t *p = &s_peers[i];
            if (strcmp(p->name, name) != 0 || p->kind != RV9_PEER_HEARD ||
                now - p->heard_ms > FORGET_MS) continue;
            memcpy(ip, p->ip, 4);
            if (http) *http = p->http;
            rv9_lock_release(s_lock);
            return true;
        }
        rv9_lock_release(s_lock);

        /* Not heard: ask now, and wait for the answer, but not for every
           lookup of a name that is not there -- once in ASK_AGAIN_MS. */
        if (pass > 0 || rv9_time_ms() - s_last_ask < ASK_AGAIN_MS) return false;
        s_answered = false;
        s_asked = true;
        for (int w = 0; w < (ASK_MS + 1000) / 20 && !s_answered; w++) rv9_task_delay_ms(20);
    }
    return false;
}

int rv9_peers_list(void *buf, uint32_t len)
{
    rv9_sys_peer_t *out = (rv9_sys_peer_t *)buf;
    uint32_t room = len / sizeof(*out), n = 0;
    if (room == 0) return 0;

    memset(&out[0], 0, sizeof(out[0]));
    strncpy(out[0].name, s_name, sizeof(out[0].name) - 1);
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (nif && esp_netif_get_ip_info(nif, &info) == ESP_OK) memcpy(out[0].ip, &info.ip.addr, 4);
    out[0].http = s_http;
    out[0].run = s_run;
    memcpy(out[0].id, s_id, 16);
    out[0].kind = RV9_PEER_THIS;
    out[0].flags = s_safe ? RV9_PEER_SAFE : 0;
    n = 1;

    rv9_lock_acquire(s_lock);
    uint64_t now = rv9_time_ms();
    for (int i = 0; i < SLOTS && n < room; i++) {
        const peer_t *p = &s_peers[i];
        if (p->name[0] == '\0' || now - p->heard_ms > FORGET_MS) continue;
        rv9_sys_peer_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        memcpy(o->name, p->name, sizeof(o->name));
        memcpy(o->ip, p->ip, 4);
        o->http = p->http;
        o->run = p->run;
        memcpy(o->id, p->id, 16);
        o->age_s = (uint32_t)((now - p->heard_ms) / 1000);
        o->kind = p->kind;
        o->flags = p->flags;
    }
    rv9_lock_release(s_lock);
    return (int)n;
}

void rv9_peers_start(void)
{
    if (s_started) return;
    if (rv9_lock_create(&s_lock) != RV9_OK) return;
    pick_name();
    if (rv9_task_create_background(discovery, "rv9-peers", TASK_STACK, NULL) != RV9_OK) {
        ESP_LOGW(TAG, "no task to find peers with");
        return;
    }
    rv9_mod_set_peers_source(rv9_peers_list);
    s_started = true;
}
