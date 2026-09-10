/*
 * scan -- list visible access points.
 *
 * Worth having on this board specifically: the C5 has a 5 GHz radio and
 * most ESP32s do not, so seeing which band a network is on answers a
 * question you cannot ask anywhere else in the family.
 */
#include "modlib.h"
#include "rv9/net.h"

typedef struct { rv9_net_scan_t scan; } scan_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return -1;

    scan_statics_t *st = (scan_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int p = env->open("/n0", RV9_MODE_READ);
    if (p < 0) { m_say(env, RV9_STDOUT, "/n0: cannot open\n"); return -3; }

    m_say(env, RV9_STDOUT, "scanning both bands...\n");
    int rc = env->getstat(p, RV9_NET_GS_SCAN, &st->scan);
    env->close(p);

    if (rc < 0) { m_say(env, RV9_STDOUT, "scan failed\n"); return -4; }

    if (st->scan.count == 0) {
        m_say(env, RV9_STDOUT, "no networks found\n");
        return 0;
    }

    m_say(env, RV9_STDOUT, "ssid                        band ch rssi auth\n");
    for (int i = 0; i < st->scan.count; i++) {
        m_pad(env, RV9_STDOUT, st->scan.ap[i].ssid, 28);
        m_say(env, RV9_STDOUT, st->scan.ap[i].band == 2 ? "5G   " : "2.4  ");
        m_numpad(env, RV9_STDOUT, st->scan.ap[i].channel, 3);
        m_numpad(env, RV9_STDOUT, st->scan.ap[i].rssi, 5);
        m_num(env, RV9_STDOUT, st->scan.ap[i].authmode);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
