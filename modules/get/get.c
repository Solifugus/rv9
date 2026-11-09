/*
 * get -- a command from another machine, kept on this one.
 *
 *   rv9> get fhello
 *   fhello: /peer/pc/cmd/fhello.mod, 460 bytes
 *     signed by solifugus; kept as /sd0/cmd/fhello.mod
 *     /sd0/fdata.bin: fetched 50,000 bytes, the build it names
 *   rv9> get listen                   (on a C5)
 *   listen: /peer/pc/cmd/listen.mod, 35,380 bytes
 *     refused, and not kept: it needs the FPU and the vector unit
 *
 * The module is the one a fork would run: found as a fork finds it, by
 * /f0/path and /f0/pins, so a pin says which peer and which build. It is
 * kept at the first place on the path that is this machine's own, where
 * the next search finds it before the peer, and it runs with the peer
 * switched off.
 *
 * Nothing is kept that this machine would refuse. The copy is made under a
 * name the path never finds and checked there by RV9_SYS_CHECK, as a module
 * from a peer is checked: it must verify, be signed by a key in /f0/trust,
 * and be able to run here. Only then does it take the module's name. The
 * files it reads (its `file` tags) come from the same place, files/ beside
 * cmd/, and each must hash to what its tag names when the tag names one --
 * which the signature covers, so a model fetched from a server nobody
 * vouches for is the one the signer built against. One already here that
 * hashes right is not fetched again, one without a hash to check is left
 * alone if it is here, and one here that is another build is left alone
 * and said: something else may read it, and which build wins is a
 * person's choice.
 *
 * The status is 0 when the module and every file it reads are here.
 */
#include "modlib.h"

#define NEEDS_MAX  1024
#define PATH_FILE  "/f0/path"
#define PATH_DEFAULT "/sd0/cmd/ /f0/cmd/"

typedef struct {
    rv9_sys_needs_t n;
    uint8_t more[NEEDS_MAX];
    rv9_sys_check_t ck;
    char rules[256];
    char place[48];             /* where it is kept: "/sd0/cmd/" */
    char dest[64];
    char temp[64];
    char root[64];              /* "/peer/p4/": where cmd/ and files/ are */
    char src[96];
    char file[64];
    char hash[65];
    uint8_t buf[1024];
} get_statics_t;

static void say(const rv9_mod_env_t *env, const char *s) { m_say(env, RV9_STDOUT, s); }
static void err(const rv9_mod_env_t *env, const char *s) { m_say(env, RV9_STDERR, s); }

static bool starts(const char *s, const char *p)
{
    while (*p) if (*s++ != *p++) return false;
    return true;
}

static void cat(char *dst, uint32_t cap, const char *a, const char *b, const char *c)
{
    uint32_t i = 0;
    const char *parts[3] = { a, b, c };
    for (int k = 0; k < 3; k++) {
        for (const char *s = parts[k]; s && *s && i < cap - 1; s++) dst[i++] = *s;
    }
    dst[i] = '\0';
}

/* 1,191,936 */
static void bytes(const rv9_mod_env_t *env, uint32_t v)
{
    char d[16];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char out[20];
    int o = 0;
    for (int i = n - 1; i >= 0; i--) {
        out[o++] = d[i];
        if (i > 0 && i % 3 == 0) out[o++] = ',';
    }
    out[o] = '\0';
    say(env, out);
}

/* The first place on the path that is this machine's, on a device it has. */
static bool keep_place(const rv9_mod_env_t *env, get_statics_t *st)
{
    int p = env->open(PATH_FILE, RV9_MODE_READ);
    int n = p >= 0 ? env->read(p, st->rules, sizeof(st->rules) - 1) : -1;
    if (p >= 0) env->close(p);
    if (n <= 0) {
        cat(st->rules, sizeof(st->rules), PATH_DEFAULT, 0, 0);
    } else {
        st->rules[n] = '\0';
    }
    for (const char *d = st->rules; *d; ) {
        d = m_word(d, st->place, sizeof(st->place));
        if (st->place[0] == '\0') break;
        if (st->place[0] != '/' || starts(st->place, "/peer/")) continue;
        char dev[16];
        uint32_t i = 0;
        while (st->place[i] && i < sizeof(dev) - 1 && (i == 0 || st->place[i] != '/')) {
            dev[i] = st->place[i];
            i++;
        }
        dev[i] = '\0';
        if (m_have(env, dev, NULL)) return true;
    }
    return false;
}

/* `from` into `to`, created or replaced. The bytes, or -1 having said why. */
static int32_t copy(const rv9_mod_env_t *env, get_statics_t *st,
                    const char *from, const char *to)
{
    int s = env->open(from, RV9_MODE_READ);
    if (s < 0) {
        err(env, from);
        err(env, ": cannot be read\n");
        return -1;
    }
    int d = env->open(to, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (d < 0) {
        env->close(s);
        err(env, to);
        err(env, ": cannot be made\n");
        return -1;
    }
    int32_t total = 0;
    for (;;) {
        int n = env->read(s, st->buf, sizeof(st->buf));
        if (n < 0) {
            err(env, from);
            err(env, ": the read failed part way\n");
            total = -1;
            break;
        }
        if (n == 0) break;
        if (env->write(d, st->buf, (uint32_t)n) != n) {
            err(env, to);
            err(env, ": ran out of room\n");
            total = -1;
            break;
        }
        total += n;
    }
    env->close(s);
    env->close(d);
    if (total < 0) env->remove(to);
    return total;
}

static int check(const rv9_mod_env_t *env, get_statics_t *st, const char *path,
                 uint32_t flags)
{
    cat(st->ck.path, sizeof(st->ck.path), path, 0, 0);
    st->ck.flags = flags;
    return env->sysinfo(RV9_SYS_CHECK, &st->ck, sizeof(st->ck));
}

static void hex(char out[65], const uint8_t sum[32])
{
    for (int i = 0; i < 32; i++) {
        out[2 * i] = "0123456789abcdef"[sum[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[sum[i] & 15];
    }
    out[64] = '\0';
}

/* Does what was just checked hash to `want`? */
static bool same_hash(get_statics_t *st, const char *want)
{
    char have[65];
    hex(have, st->ck.sha256);
    return m_eq(have, want);
}

/*
 * One file the module reads: "/sd0/whistle.cact", or with the hash it must
 * have, "/sd0/whistle.cact=b6e02f04...". 0 when it is here as it must be.
 */
static int one_file(const rv9_mod_env_t *env, get_statics_t *st,
                    const uint8_t *v, uint16_t len)
{
    uint16_t i = 0;
    while (i < len && v[i] != '=' && i < sizeof(st->file) - 1) { st->file[i] = (char)v[i]; i++; }
    st->file[i] = '\0';
    st->hash[0] = '\0';
    if (i < len && v[i] == '=' && len - i - 1 == 64) {
        for (uint16_t k = 0; k < 64; k++) st->hash[k] = (char)v[i + 1 + k];
        st->hash[64] = '\0';
    }

    say(env, "  ");
    say(env, st->file);
    say(env, ": ");
    if (!starts(st->file, "/sd0/")) {
        say(env, "not on a card; left to whoever puts it there\n");
        return 0;
    }

    int have = check(env, st, st->file, 0);
    if (have == 1 && (st->hash[0] == '\0' || same_hash(st, st->hash))) {
        say(env, st->hash[0] ? "here, the build it names\n" : "here\n");
        return 0;
    }
    if (have == 1) {
        /* Another build, which some other module here may read: two that
           want different builds of one file is for a person to settle. */
        say(env, "here, and another build than it names\n");
        err(env, "  left alone, as something else may read it; remove it and get "
                 "again to replace it\n");
        return -1;
    }

    /* files/ beside the cmd/ the module came from: what httpd serves, and
       what tools/mkrepo.py writes. */
    if (st->root[0] == '\0') {
        say(env, "not fetched: its module came from no cmd/ to look beside\n");
        return -1;
    }
    cat(st->src, sizeof(st->src), st->root, "files/", st->file + 5);
    int32_t got = copy(env, st, st->src, st->file);
    if (got < 0) return -1;
    say(env, "fetched ");
    bytes(env, (uint32_t)got);
    say(env, " bytes");
    if (st->hash[0] == '\0') {
        say(env, "; its module names no hash to check it by\n");
        return 0;
    }
    if (check(env, st, st->file, 0) != 1 || !same_hash(st, st->hash)) {
        env->remove(st->file);
        say(env, "\n");
        err(env, "  not the build its module names, and removed\n");
        return -1;
    }
    say(env, ", the build it names\n");
    return 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 17) return 2;     /* RV9_SYS_CHECK */
    get_statics_t *st = (get_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        err(env, "get: static_size in build.conf is too small\n");
        return 2;
    }

    char name[24];
    m_word(env->arg ? env->arg : "", name, sizeof(name));
    if (name[0] == '\0') {
        err(env, "usage: get <command>\n");
        return 2;
    }

    cat((char *)&st->n, sizeof(st->n.from), name, 0, 0);
    if (env->sysinfo(RV9_SYS_NEEDS, &st->n, sizeof(st->n) + sizeof(st->more)) != 1) {
        err(env, name);
        err(env, ": no such command here or on the path\n");
        return 1;
    }
    if (!starts(st->n.from, "/peer/")) {
        say(env, name);
        say(env, st->n.from[0] ? ": here already, " : ": here already, in the store\n");
        if (st->n.from[0]) { say(env, st->n.from); say(env, "\n"); }
        return 0;
    }
    if (!keep_place(env, st)) {
        err(env, "get: no place on /f0/path is on this machine\n");
        return 1;
    }

    /* "/peer/p4/cmd/greet.mod": the root is what comes before cmd/. */
    uint32_t root_len = m_len(st->n.from);
    while (root_len > 0 && !starts(st->n.from + root_len, "cmd/")) root_len--;
    st->root[0] = '\0';
    if (root_len > 0 && root_len < sizeof(st->root)) {
        for (uint32_t i = 0; i < root_len; i++) st->root[i] = st->n.from[i];
        st->root[root_len] = '\0';
    }

    cat(st->dest, sizeof(st->dest), st->place, name, ".mod");
    cat(st->temp, sizeof(st->temp), st->place, name, ".new");
    say(env, name);
    say(env, ": ");
    say(env, st->n.from);
    int32_t got = copy(env, st, st->n.from, st->temp);
    if (got < 0) return 1;
    say(env, ", ");
    bytes(env, (uint32_t)got);
    say(env, " bytes\n");

    if (check(env, st, st->temp, RV9_CHECK_MODULE) != 1) {
        env->remove(st->temp);
        err(env, "  refused, and not kept: ");
        err(env, st->ck.lacks[0] ? "it needs " :
                 st->ck.err == RV9_MOD_ERR_SIGNATURE ? "no key in /f0/trust vouches for it" :
                 st->ck.err == RV9_MOD_ERR_NOMEM ? "no memory to check it in" :
                 "not a whole module");
        err(env, st->ck.lacks);
        err(env, "; `log` says more\n");
        return 1;
    }
    if (copy(env, st, st->temp, st->dest) < 0) {
        env->remove(st->temp);
        return 1;
    }
    env->remove(st->temp);
    say(env, "  signed by ");
    say(env, st->ck.signer);
    say(env, "; kept as ");
    say(env, st->dest);
    say(env, "\n");

    /* The files it reads, from the manifest of the copy just checked -- the
       search's came from the peer unchecked, and it is the signed one whose
       hashes are worth believing. Kept, it is what the search finds now. */
    cat((char *)&st->n, sizeof(st->n.from), name, 0, 0);
    if (env->sysinfo(RV9_SYS_NEEDS, &st->n, sizeof(st->n) + sizeof(st->more)) != 1 ||
        !m_eq(st->n.from, st->dest)) {
        err(env, "  its files not fetched: the peer's copy is in use; get it again "
                 "when nothing runs it\n");
        return 1;
    }
    int bad = 0;
    const uint8_t *m = st->n.manifest;
    for (uint32_t off = 0; off + 4 <= st->n.len; ) {
        uint16_t tag = (uint16_t)(m[off] | (m[off + 1] << 8));
        uint16_t len = (uint16_t)(m[off + 2] | (m[off + 3] << 8));
        if (RV9_MTAG_NUMBER(tag) == RV9_MTAG_END || off + 4 + len > st->n.len) break;
        if (RV9_MTAG_NUMBER(tag) == RV9_MTAG_FILE && one_file(env, st, m + off + 4, len) < 0) {
            bad++;
        }
        uint32_t next = off + 4 + len;
        off = next + (4 - (next % 4)) % 4;
    }
    return bad ? 1 : 0;
}
