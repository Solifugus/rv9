/*
 * trust -- the keys whose signed modules this machine runs from a file.
 *
 *   trust                   who is trusted
 *   trust add [line]        trust a key: the line tools/mkkey.py prints,
 *                           given here or pasted at the prompt
 *   trust remove <who>      by name or key id
 *
 * Keys live in /f0/trust, one per line: name, key id, public key. With
 * none there, a module read from a file runs whether or not it is signed,
 * with a warning; with one, it must be signed by a key in the file. The
 * store is trusted either way: writing it is flashing the board. See
 * rv9_mod_sig_t in rv9/module.h.
 *
 * Anything that can write /f0 can change this file, so trust here is as
 * strong as who can reach a shell. That is written down rather than
 * pretended away: RV-9 has no privileges yet.
 */
#include "modlib.h"

#define TRUST_PATH "/f0/trust"
#define TRUST_MAX  2048            /* the module manager reads this much */

typedef struct {
    char file[TRUST_MAX + 1];
    char line[256];
} trust_statics_t;

static bool is_hex(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/* The fields of one line: name, id, key. False if it is not a key line. */
static bool fields(const char *line, uint32_t len, const char *f[3], uint32_t fl[3])
{
    uint32_t i = 0;
    for (int k = 0; k < 3; k++) {
        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
        f[k] = line + i;
        while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
        fl[k] = (uint32_t)(line + i - f[k]);
    }
    return fl[0] > 0 && fl[0] < 24 && f[0][0] != '#' &&
           fl[1] == 16 && is_hex(f[1], 16) &&
           fl[2] == 130 && f[2][0] == '0' && f[2][1] == '4' && is_hex(f[2], 130);
}

/* No libc, so no memcmp: a module has only what it brings. */
static bool eq_n(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

static bool same(const char *a, uint32_t al, const char *b)
{
    return al == m_len(b) && eq_n(a, b, al);
}

static int load(const rv9_mod_env_t *env, trust_statics_t *st)
{
    int n = 0;
    int p = env->open(TRUST_PATH, RV9_MODE_READ);
    if (p >= 0) {
        for (;;) {
            int got = env->read(p, st->file + n, (uint32_t)(TRUST_MAX - n));
            if (got <= 0) break;
            n += got;
        }
        env->close(p);
    }
    st->file[n] = '\0';
    return n;
}

static int save(const rv9_mod_env_t *env, const char *text, uint32_t len)
{
    int p = env->open(TRUST_PATH, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (p < 0) return -1;
    int w = len ? env->write(p, text, len) : 0;
    env->close(p);
    return w == (int)len ? 0 : -1;
}

static void list(const rv9_mod_env_t *env, trust_statics_t *st)
{
    int n = load(env, st);
    int keys = 0;
    for (int i = 0; i < n; ) {
        int e = i;
        while (e < n && st->file[e] != '\n') e++;
        const char *f[3];
        uint32_t fl[3];
        if (fields(st->file + i, (uint32_t)(e - i), f, fl)) {
            if (keys++ == 0) m_say(env, RV9_STDOUT, "name                    key id\n");
            char name[24], id[17];
            for (uint32_t k = 0; k < fl[0]; k++) name[k] = f[0][k];
            name[fl[0]] = '\0';
            for (uint32_t k = 0; k < 16; k++) id[k] = f[1][k];
            id[16] = '\0';
            m_pad(env, RV9_STDOUT, name, 24);
            m_say(env, RV9_STDOUT, id);
            m_say(env, RV9_STDOUT, "\n");
        }
        i = e + 1;
    }
    if (keys == 0) {
        m_say(env, RV9_STDOUT, "no keys: modules from files run signed or not, "
                               "with a warning\n");
    }
}

static int add(const rv9_mod_env_t *env, trust_statics_t *st, const char *given)
{
    uint32_t n = 0;
    if (given != NULL && given[0] != '\0') {
        while (given[n] && n < sizeof(st->line) - 1) { st->line[n] = given[n]; n++; }
    } else {
        m_say(env, RV9_STDOUT, "paste the line tools/mkkey.py printed:\n");
        int got = env->read(RV9_STDIN, st->line, sizeof(st->line) - 1);
        if (got <= 0) { m_say(env, RV9_STDOUT, "nothing read\n"); return 3; }
        n = (uint32_t)got;
    }
    while (n > 0 && (st->line[n - 1] == '\n' || st->line[n - 1] == '\r' ||
                     st->line[n - 1] == ' ')) n--;
    st->line[n] = '\0';

    const char *f[3];
    uint32_t fl[3];
    if (!fields(st->line, n, f, fl)) {
        m_say(env, RV9_STDOUT, "that is not a key line: <name> <16 hex> <130 hex>\n");
        return 4;
    }

    int len = load(env, st);
    for (int i = 0; i < len; ) {
        int e = i;
        while (e < len && st->file[e] != '\n') e++;
        const char *g[3];
        uint32_t gl[3];
        if (fields(st->file + i, (uint32_t)(e - i), g, gl) &&
            eq_n(g[1], f[1], 16)) {
            m_say(env, RV9_STDOUT, "already trusted\n");
            return 0;
        }
        i = e + 1;
    }
    if (len + (int)n + 2 > TRUST_MAX) {
        m_say(env, RV9_STDOUT, "/f0/trust is full\n");
        return 5;
    }
    if (len > 0 && st->file[len - 1] != '\n') st->file[len++] = '\n';
    for (uint32_t k = 0; k < n; k++) st->file[len++] = st->line[k];
    st->file[len++] = '\n';
    if (save(env, st->file, (uint32_t)len) < 0) {
        m_say(env, RV9_STDOUT, TRUST_PATH ": cannot write\n");
        return 6;
    }
    m_say(env, RV9_STDOUT, "trusted; modules from files must now be signed by "
                           "a key in " TRUST_PATH "\n");
    return 0;
}

static int remove_key(const rv9_mod_env_t *env, trust_statics_t *st, const char *who)
{
    int len = load(env, st), out = 0, gone = 0;
    for (int i = 0; i < len; ) {
        int e = i;
        while (e < len && st->file[e] != '\n') e++;
        const char *f[3];
        uint32_t fl[3];
        bool match = fields(st->file + i, (uint32_t)(e - i), f, fl) &&
                     (same(f[0], fl[0], who) || same(f[1], fl[1], who));
        if (match) {
            gone++;
        } else {
            for (int k = i; k < e; k++) st->file[out++] = st->file[k];
            if (e < len) st->file[out++] = '\n';
        }
        i = e + 1;
    }
    if (gone == 0) {
        m_say(env, RV9_STDOUT, "no such key\n");
        return 1;
    }
    if (save(env, st->file, (uint32_t)out) < 0) {
        m_say(env, RV9_STDOUT, TRUST_PATH ": cannot write\n");
        return 6;
    }
    m_say(env, RV9_STDOUT, "removed\n");
    return 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return 2;

    trust_statics_t *st = (trust_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    char verb[8];
    const char *rest = "";
    verb[0] = '\0';
    if (env->arg != NULL) rest = m_word(env->arg, verb, sizeof(verb));
    while (*rest == ' ') rest++;

    if (verb[0] == '\0') { list(env, st); return 0; }
    if (m_eq(verb, "add")) return add(env, st, rest);
    if (m_eq(verb, "remove") && rest[0] != '\0') return remove_key(env, st, rest);

    m_say(env, RV9_STDERR, "usage: trust | trust add [line] | trust remove <name or id>\n");
    return 2;
}
