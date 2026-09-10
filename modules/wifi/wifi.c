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
        m_say(env, RV9_STDOUT, "usage: wifi <ssid> <password>\n");
        return -3;
    }

    /* Split the argument on its first space. */
    uint32_t i = 0, j = 0;
    while (env->arg[i] && env->arg[i] != ' ' && j < sizeof(st->creds.ssid) - 1) {
        st->creds.ssid[j++] = env->arg[i++];
    }
    st->creds.ssid[j] = '\0';
    while (env->arg[i] == ' ') i++;
    j = 0;
    while (env->arg[i] && j < sizeof(st->creds.pass) - 1) {
        st->creds.pass[j++] = env->arg[i++];
    }
    st->creds.pass[j] = '\0';

    int p = env->open("/n0", RV9_MODE_WRITE);
    if (p < 0) { m_say(env, RV9_STDOUT, "/n0: cannot open\n"); return -4; }

    int rc = env->setstat(p, RV9_NET_SS_CONNECT, &st->creds);
    env->close(p);

    if (rc < 0) { m_say(env, RV9_STDOUT, "connect refused\n"); return -5; }

    m_say(env, RV9_STDOUT, "associating with ");
    m_say(env, RV9_STDOUT, st->creds.ssid);
    m_say(env, RV9_STDOUT, "; check with netstat\n");
    return 0;
}
