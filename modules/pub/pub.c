/*
 * pub -- publish a set of values, once.
 *
 * The smallest thing that can be on the writing end of a publication
 * cell, and the counterpart of `watch`. A control loop publishes what it
 * measured; this publishes what you typed, which is what makes the whole
 * arrangement testable from a shell without a machine attached.
 *
 *     pub SPEED 1200 40 7      creates /pub0/SPEED and publishes three
 *                              values as one coherent set
 *
 * It exits immediately afterwards, and the cell keeps what it was given.
 * That is deliberate rather than convenient: a publication outlives its
 * publisher, so the last thing a stopped component said is still readable.
 */
#include "modlib.h"

#define MAX_VALUES 12
#define DEV        "/pub0/"

typedef struct {
    char     path[48];
    uint8_t  msg[sizeof(rv9_pub_t) + MAX_VALUES * sizeof(int32_t)];
} pub_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 11) return -1;

    pub_statics_t *st = (pub_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: pub <name> <value> [value ...]\n"
                               "  publishes to /pub0/<name>; see 'pubs'\n");
        return -3;
    }

    char name[RV9_PUB_MAX_NAME];
    const char *rest = m_word(env->arg, name, sizeof(name));

    /* A pointer to the literal, never an array initialised from one: the
       second compiles to a memcpy a module has no libc to supply. */
    const char *dev = DEV;
    uint32_t i = 0;
    while (*dev && i < sizeof(st->path) - 1) st->path[i++] = *dev++;
    for (uint32_t k = 0; name[k] && i < sizeof(st->path) - 1; k++) {
        st->path[i++] = name[k];
    }
    st->path[i] = '\0';

    /*
     * The value set, laid out as it will be published: the head, then the
     * values, one write. Building it in place is the point -- there is no
     * moment when half of it is visible, because nothing is visible until
     * the single write lands.
     */
    rv9_pub_t *head = (rv9_pub_t *)st->msg;
    int32_t *val = (int32_t *)(st->msg + sizeof(rv9_pub_t));

    uint32_t n = 0;
    while (*rest && n < MAX_VALUES) {
        bool neg = (*rest == '-');
        if (neg) rest++;
        const char *end = rest;
        uint32_t v = m_num_parse(rest, &end);
        if (end == rest) break;
        val[n++] = neg ? -(int32_t)v : (int32_t)v;
        rest = end;
        while (*rest == ' ') rest++;
    }

    if (n == 0) {
        m_say(env, RV9_STDOUT, "pub: nothing to publish\n");
        return -4;
    }

    int p = env->open(st->path, RV9_MODE_WRITE);
    if (p < 0) {
        m_say(env, RV9_STDOUT, st->path);
        if (p == -RV9_IOE_BUSY) {
            m_say(env, RV9_STDOUT, ": something else is publishing it\n");
        } else if (p == -RV9_IOE_NOMEM) {
            m_say(env, RV9_STDOUT, ": no cell free (see 'pubs')\n");
        } else if (p == -RV9_IOE_INVAL) {
            m_say(env, RV9_STDOUT, ": not a usable publication name\n");
        } else {
            m_say(env, RV9_STDOUT, ": cannot open\n");
        }
        return -5;
    }

    head->seq      = 0;              /* the cell owns this */
    head->len      = n * sizeof(int32_t);
    head->stamp_us = 0;              /* zero means "observed now" */

    int wrote = env->write(p, st->msg, sizeof(rv9_pub_t) + head->len);
    env->close(p);

    if (wrote < 0) {
        m_say(env, RV9_STDOUT, "pub: the cell refused it\n");
        return -6;
    }

    m_say(env, RV9_STDOUT, st->path);
    m_say(env, RV9_STDOUT, " <- ");
    for (uint32_t k = 0; k < n; k++) {
        if (k) m_say(env, RV9_STDOUT, " ");
        m_num(env, RV9_STDOUT, val[k]);
    }
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
