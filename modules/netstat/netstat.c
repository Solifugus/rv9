/*
 * netstat -- report the link.
 */
#include "modlib.h"
#include "rv9/net.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return -1;

    int p = env->open("/n0", RV9_MODE_READ);
    if (p < 0) { m_say(env, RV9_STDOUT, "/n0: cannot open\n"); return -2; }

    rv9_net_status_t st;
    int rc = env->getstat(p, RV9_NET_GS_STATUS, &st);
    env->close(p);
    if (rc < 0) { m_say(env, RV9_STDOUT, "no status\n"); return -3; }

    m_say(env, RV9_STDOUT, "state   ");
    if (st.state == RV9_NET_UP)              m_say(env, RV9_STDOUT, "up");
    else if (st.state == RV9_NET_CONNECTING) m_say(env, RV9_STDOUT, "connecting");
    else if (st.state == RV9_NET_FAILED)     m_say(env, RV9_STDOUT, "failed");
    else                                     m_say(env, RV9_STDOUT, "down (loopback only)");

    m_say(env, RV9_STDOUT, "\nssid    ");
    m_say(env, RV9_STDOUT, st.ssid[0] ? st.ssid : "-");

    m_say(env, RV9_STDOUT, "\naddress ");
    if (st.ip == 0) {
        m_say(env, RV9_STDOUT, "-");
    } else {
        for (int i = 0; i < 4; i++) {
            if (i) m_say(env, RV9_STDOUT, ".");
            m_num(env, RV9_STDOUT, (int32_t)((st.ip >> (8 * i)) & 0xFF));
        }
    }
    if (st.reason) {
        m_say(env, RV9_STDOUT, "\nlast reason ");
        m_num(env, RV9_STDOUT, st.reason);
        if (st.reason == 201) m_say(env, RV9_STDOUT, " (no such AP)");
        else if (st.reason == 202) m_say(env, RV9_STDOUT, " (auth failed)");
        else if (st.reason == 15)  m_say(env, RV9_STDOUT, " (4-way handshake timeout)");
        else if (st.reason == 205) m_say(env, RV9_STDOUT, " (connection failed)");
    }
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
