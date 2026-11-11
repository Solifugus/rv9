/*
 * peers -- the other boards, as this one knows them.
 *
 *   rv9> peers
 *   this board  c5            192.168.1.121    a546321e
 *   name        address          heard  id        serves  runs
 *   p4          192.168.1.178    12 s   b2630658  8080    2301
 *   c3-5852a8   192.168.1.145    3 s    73ff954b  -       -
 *
 * Every RV-9 board announces itself by mDNS and listens for the others
 * (design section 81), so this is who answered in the last couple of
 * minutes: its address, how long since it was heard, the start of its id
 * (what `ident` prints there), and the ports of its httpd and execd, if it
 * runs them. A name heard with an id other than the one first heard with
 * it is shown `refused`, and /peer does not use it; /f0/known is where the
 * first ids are kept. Then the lines of /f0/peers, which win over what is
 * heard.
 */
#include "modlib.h"

#define MAX 17

typedef struct {
    rv9_sys_peer_t p[MAX];
    char line[96];
    char ip[16];
    char id[9];
    m_lines_t lines;
} peers_statics_t;

static void addr(peers_statics_t *st, const uint8_t ip[4])
{
    uint32_t n = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t v = ip[k];
        char d[3];
        int c = 0;
        do { d[c++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (c) st->ip[n++] = d[--c];
        if (k < 3) st->ip[n++] = '.';
    }
    st->ip[n] = '\0';
}

static void idhex(peers_statics_t *st, const uint8_t id[16])
{
    for (int i = 0; i < 4; i++) {
        st->id[2 * i]     = "0123456789abcdef"[id[i] >> 4];
        st->id[2 * i + 1] = "0123456789abcdef"[id[i] & 15];
    }
    st->id[8] = '\0';
}

static void port(const rv9_mod_env_t *env, uint32_t v, uint32_t w)
{
    if (v) m_numpad(env, RV9_STDOUT, (int32_t)v, w);
    else m_pad(env, RV9_STDOUT, "-", w);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 19) return 2;   /* RV9_SYS_PEERS */

    peers_statics_t *st = (peers_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    int n = env->sysinfo(RV9_SYS_PEERS, st->p, sizeof(st->p));
    if (n <= 0) {
        m_say(env, RV9_STDOUT, "this board does not look for peers (no network)\n");
    } else {
        rv9_sys_peer_t *me = &st->p[0];
        me->name[15] = '\0';
        addr(st, me->ip);
        idhex(st, me->id);
        m_say(env, RV9_STDOUT, "this board  ");
        m_pad(env, RV9_STDOUT, me->name, 14);
        m_pad(env, RV9_STDOUT, st->ip, 17);
        m_say(env, RV9_STDOUT, st->id);
        m_say(env, RV9_STDOUT, "\n");
        if (n == 1) {
            m_say(env, RV9_STDOUT, "no other board heard yet\n");
        } else {
            m_say(env, RV9_STDOUT, "name        address          heard  id        serves  runs\n");
        }
        for (int i = 1; i < n; i++) {
            rv9_sys_peer_t *p = &st->p[i];
            p->name[15] = '\0';
            addr(st, p->ip);
            idhex(st, p->id);
            m_pad(env, RV9_STDOUT, p->name, 12);
            m_pad(env, RV9_STDOUT, st->ip, 17);
            {
                /* "12 s", padded as one: the number's width varies */
                char age[12];
                uint32_t v = p->age_s, k = 0;
                char d[10];
                int c = 0;
                do { d[c++] = (char)('0' + v % 10); v /= 10; } while (v);
                while (c) age[k++] = d[--c];
                age[k++] = ' ';
                age[k++] = 's';
                age[k] = '\0';
                m_pad(env, RV9_STDOUT, age, 7);
            }
            m_pad(env, RV9_STDOUT, st->id, 10);
            if (p->kind == RV9_PEER_REFUSED) {
                m_say(env, RV9_STDOUT, "refused: not the id first heard (/f0/known)\n");
                continue;
            }
            port(env, p->http, 8);
            port(env, p->run, 6);
            m_say(env, RV9_STDOUT, "\n");
        }
    }

    int f = env->open("/f0/peers", RV9_MODE_READ);
    if (f >= 0) {
        bool said = false;
        while (m_getline(env, f, &st->lines, st->line, sizeof(st->line)) >= 0) {
            if (st->line[0] == '\0' || st->line[0] == '#') continue;
            if (!said) m_say(env, RV9_STDOUT, "in /f0/peers, which wins:\n");
            said = true;
            m_say(env, RV9_STDOUT, "  ");
            m_say(env, RV9_STDOUT, st->line);
            m_say(env, RV9_STDOUT, "\n");
        }
        env->close(f);
    }
    return 0;
}
