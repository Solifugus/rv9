/*
 * can -- could this machine run it, and if not, why not.
 *
 *   rv9> can listen                 (on a C5, with listen on its card)
 *   listen, from /sd0/cmd/listen.mod
 *     --   uses /au0              only for some of what it does
 *     no   file /sd0/whistle.cact not there
 *     no   isa fpu pie            this CPU has neither
 *     no   memory 28160 KB external this machine has none
 *   no: it would be refused
 *
 * It reads the module's manifest as a fork would find the module (the
 * store, then the module path) and checks every need, not only the first
 * one that fails. A device, an exclusive one, or an instruction set it
 * declared are what admission refuses on. A module it starts, a file it
 * reads and external memory for its arena are not refused on, but it would
 * fail without them, so they count against "yes" too. A device it only
 * `uses` is shown and never counts: some of what it does works without.
 *
 * Silent but for the report; the answer is also the status, 0 for yes.
 */
#include "modlib.h"

#define NEEDS_MAX 1024

typedef struct {
    rv9_sys_needs_t n;
    uint8_t more[NEEDS_MAX];
    rv9_sys_needs_t probe;          /* for a module it starts */
    uint8_t probe_more[4];
    rv9_sys_limits_t lim;
    rv9_sys_mem_t mem;
    char word[64];
} can_statics_t;

static void line(const rv9_mod_env_t *env, const char *verdict,
                 const char *what, const char *value, const char *why)
{
    m_say(env, RV9_STDOUT, "  ");
    m_pad(env, RV9_STDOUT, verdict, 5);
    m_say(env, RV9_STDOUT, what);
    m_say(env, RV9_STDOUT, " ");
    if (why != NULL && why[0] != '\0') {
        m_pad(env, RV9_STDOUT, value, 22 - (int)m_len(what));
        m_say(env, RV9_STDOUT, why);
    } else {
        m_say(env, RV9_STDOUT, value);
    }
    m_say(env, RV9_STDOUT, "\n");
}

/* A manifest string, which carries its length and no terminator. */
static void copy_value(char *out, uint32_t cap, const uint8_t *v, uint16_t len)
{
    uint32_t n = len < cap - 1 ? len : cap - 1;
    for (uint32_t i = 0; i < n; i++) out[i] = (char)v[i];
    out[n] = '\0';
}

/* The device a path is on: "/tsens/0" is /tsens. */
static void device_of(char *dev, uint32_t cap, const char *path)
{
    uint32_t i = 0;
    while (path[i] != '\0' && i < cap - 1 && (i == 0 || path[i] != '/')) {
        dev[i] = path[i];
        i++;
    }
    dev[i] = '\0';
}

static bool found(const rv9_mod_env_t *env, can_statics_t *st, const char *name)
{
    uint32_t i = 0;
    while (name[i] != '\0' && i < sizeof(st->probe.from) - 1) {
        ((char *)&st->probe)[i] = name[i];
        i++;
    }
    ((char *)&st->probe)[i] = '\0';
    return env->sysinfo(RV9_SYS_NEEDS, &st->probe,
                        sizeof(st->probe) + sizeof(st->probe_more)) == 1;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 16) return 2;   /* RV9_SYS_NEEDS */

    can_statics_t *st = (can_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    char name[32];
    name[0] = '\0';
    if (env->arg != NULL) m_word(env->arg, name, sizeof(name));
    if (name[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: can <command>\n");
        return 2;
    }

    for (uint32_t i = 0; i <= m_len(name); i++) ((char *)&st->n)[i] = name[i];
    if (env->sysinfo(RV9_SYS_NEEDS, &st->n, sizeof(st->n) + sizeof(st->more)) != 1) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": no such command here, in the store or on the path\n");
        return 1;
    }

    uint32_t isa = 0;
    if (env->sysinfo(RV9_SYS_LIMITS, &st->lim, sizeof(st->lim)) == 1) isa = st->lim.isa;
    uint32_t ext_largest = 0;
    if (env->sysinfo(RV9_SYS_MEM, &st->mem, sizeof(st->mem)) == 1) {
        ext_largest = st->mem.ext_largest;
    }

    m_say(env, RV9_STDOUT, name);
    if (st->n.from[0] == '\0') {
        m_say(env, RV9_STDOUT, ", in the store\n");
    } else {
        m_say(env, RV9_STDOUT, ", from ");
        m_say(env, RV9_STDOUT, st->n.from);
        m_say(env, RV9_STDOUT, "\n");
    }

    bool refused = false, missing = false, said = false;
    uint32_t heap_max = 0;
    uint8_t heap_in = 0;

    const uint8_t *m = st->n.manifest;
    uint32_t off = 0;
    while (off + 4 <= st->n.len) {
        uint16_t tag = (uint16_t)(m[off] | (m[off + 1] << 8));
        uint16_t len = (uint16_t)(m[off + 2] | (m[off + 3] << 8));
        const uint8_t *v = m + off + 4;
        if (RV9_MTAG_NUMBER(tag) == RV9_MTAG_END || off + 4 + len > st->n.len) break;
        uint32_t u32 = len == 4 ? (uint32_t)(v[0] | (v[1] << 8) | (v[2] << 16) |
                                             ((uint32_t)v[3] << 24)) : 0;

        switch (RV9_MTAG_NUMBER(tag)) {
        case RV9_MTAG_DEVICE:
        case RV9_MTAG_EXCLUSIVE:
        case RV9_MTAG_USES: {
            char dev[16];
            copy_value(st->word, sizeof(st->word), v, len);
            device_of(dev, sizeof(dev), st->word);
            bool have = m_have(env, dev, NULL);
            bool must = RV9_MTAG_NUMBER(tag) != RV9_MTAG_USES;
            if (!have && must) refused = true;
            line(env, have ? "ok" : (must ? "no" : "--"),
                 must ? "device" : "uses", st->word,
                 have || must ? "" : "only for some of what it does");
            said = true;
            break;
        }
        case RV9_MTAG_ISA: {
            uint32_t lack = u32 & ~isa;
            char *w = st->word;
            uint32_t k = 0;
            if (u32 & RV9_ISA_FPU) { for (const char *s = "fpu "; *s; s++) w[k++] = *s; }
            if (u32 & RV9_ISA_PIE) { for (const char *s = "pie "; *s; s++) w[k++] = *s; }
            if (k) k--;
            w[k] = '\0';
            if (lack) refused = true;
            line(env, lack ? "no" : "ok", "isa", w,
                 lack == u32 && (u32 & (u32 - 1)) ? "this CPU has neither"
                 : lack ? "this CPU lacks some of it" : "");
            said = true;
            break;
        }
        case RV9_MTAG_MODULE: {
            copy_value(st->word, sizeof(st->word), v, len);
            bool have = found(env, st, st->word);
            if (!have) missing = true;
            line(env, have ? "ok" : "no", "starts", st->word,
                 have ? "" : "not here");
            said = true;
            break;
        }
        case RV9_MTAG_FILE: {
            /* "/sd0/whistle.cact=<sha256>": the file, and the build it must be */
            uint16_t fl = 0;
            while (fl < len && v[fl] != '=') fl++;
            copy_value(st->word, sizeof(st->word), v, fl);
            int p = env->open(st->word, RV9_MODE_READ);
            if (p >= 0) env->close(p);
            if (p < 0) missing = true;
            line(env, p >= 0 ? "ok" : "no", "file", st->word,
                 p >= 0 ? "" : "not there");
            said = true;
            break;
        }
        case RV9_MTAG_HEAP_MAX:
            heap_max = u32;
            break;
        case RV9_MTAG_HEAP_IN:
            if (len == 1) heap_in = v[0];
            break;
        default:
            break;
        }

        uint32_t next = off + 4 + len;
        next += (4 - (next % 4)) % 4;
        off = next;
    }

    if (heap_in == RV9_HEAP_EXTERNAL && heap_max > 0) {
        bool room = ext_largest >= heap_max;
        if (!room) missing = true;
        uint32_t i = 0;
        uint32_t kb = (heap_max + 1023) / 1024;
        char digits[12];
        uint32_t nd = 0;
        do { digits[nd++] = (char)('0' + kb % 10); kb /= 10; } while (kb && nd < sizeof(digits));
        while (nd) st->word[i++] = digits[--nd];
        for (const char *s = " KB external"; *s; s++) st->word[i++] = *s;
        st->word[i] = '\0';
        line(env, room ? "ok" : "no", "memory", st->word,
             room ? "" : (ext_largest ? "more than is free in one piece"
                                      : "this machine has none"));
        said = true;
    }

    if (!said) m_say(env, RV9_STDOUT, "  it declares no needs\n");

    if (refused) {
        m_say(env, RV9_STDOUT, "no: it would be refused\n");
        return 1;
    }
    if (missing) {
        m_say(env, RV9_STDOUT, "no: it would start, and lack what it needs\n");
        return 1;
    }
    m_say(env, RV9_STDOUT, "yes\n");
    return 0;
}
