/*
 * wifi -- associate with an access point.  wifi <ssid> <password>
 *
 * Credentials are typed by whoever owns the network. They are not in any
 * source file, descriptor or flash image put here by anyone else.
 */
#include "modlib.h"
#include "rv9/net.h"

typedef struct { rv9_net_creds_t creds; } wifi_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return -1;

    wifi_statics_t *st = (wifi_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: wifi <ssid> <password>\n"
                               "       wifi try <ssid> <password>   (not saved)\n"
                               "       wifi forget\n");
        return -3;
    }

    if (m_eq(env->arg, "forget")) {
        int fp = env->open("/n0", RV9_MODE_WRITE);
        if (fp < 0) return -4;
        int frc = env->setstat(fp, RV9_NET_SS_FORGET, 0);
        env->close(fp);
        m_say(env, RV9_STDOUT, frc < 0 ? "could not forget\n"
                                       : "forgot the saved network\n");
        return frc < 0 ? -5 : 0;
    }

    /* `wifi try ...` connects without saving: the saved network stays the
       one the board returns to, after a failure or a reboot. */
    const char *a = env->arg;
    bool trying = (a[0] == 't' && a[1] == 'r' && a[2] == 'y' && a[3] == ' ');
    if (trying) {
        a += 4;
        while (*a == ' ') a++;
    }

    /* Split the argument on its first space. */
    uint32_t i = 0, j = 0;
    while (a[i] && a[i] != ' ' && j < sizeof(st->creds.ssid) - 1) {
        st->creds.ssid[j++] = a[i++];
    }
    st->creds.ssid[j] = '\0';
    while (a[i] == ' ') i++;
    j = 0;
    while (a[i] && j < sizeof(st->creds.pass) - 1) {
        st->creds.pass[j++] = a[i++];
    }
    st->creds.pass[j] = '\0';

    int p = env->open("/n0", RV9_MODE_WRITE);
    if (p < 0) { m_say(env, RV9_STDOUT, "/n0: cannot open\n"); return -4; }

    int rc = env->setstat(p, trying ? RV9_NET_SS_TRY : RV9_NET_SS_CONNECT,
                          &st->creds);
    env->close(p);

    if (rc < 0) { m_say(env, RV9_STDOUT, "connect refused\n"); return -5; }

    m_say(env, RV9_STDOUT, "associating with ");
    m_say(env, RV9_STDOUT, st->creds.ssid);
    m_say(env, RV9_STDOUT, trying
          ? "; check with netstat\n"
            "not saved: if it fails, or on reboot, the saved network is used\n"
          : "; check with netstat\n"
            "saved to flash; it will reconnect on boot\n");
    return 0;
}
