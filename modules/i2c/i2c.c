/*
 * i2c -- find what is on the bus, and read a register.
 *
 * NOTE ON WHAT THIS RETURNS
 *
 * Positive, always. A module's exit status shares a number space with the
 * process manager's own verdicts -- RV9_PE_FAULT is 6, RV9_PE_DEADLINE is
 * 13 -- so a module returning -6 is reported by the shell as having been
 * stopped by the scheduler. This one did exactly that, ran perfectly, and
 * was announced as having crashed.
 *
 *   i2c scan             which addresses answer
 *   i2c 0x68 0x75        read one byte of register 0x75
 *   i2c 0x68 0x3b 6      read six bytes, in one transaction
 *
 * `scan` is the first thing anybody needs and the thing that is miserable
 * without a tool: a sensor that does not answer is indistinguishable from
 * a sensor wired to the wrong pins, and the only way to tell is to ask
 * every address and see who replies.
 *
 * Reading several bytes matters as much. Six bytes from an inertial unit
 * are three axes from one instant; fetched one at a time they are three
 * axes from three instants, and the vector they describe never existed.
 * The register form here is a single transaction for exactly that reason.
 */
#include "modlib.h"

#define MAX_READ 32

typedef struct { uint8_t buf[MAX_READ]; } i2c_statics_t;

static char nyb(uint8_t v)
{
    v &= 0xF;
    return (char)((v < 10) ? ('0' + v) : ('a' + (v - 10)));
}

static void hex2(const rv9_mod_env_t *env, uint8_t v)
{
    char out[3];
    out[0] = nyb((uint8_t)(v >> 4));
    out[1] = nyb(v);
    out[2] = '\0';
    m_say(env, RV9_STDOUT, out);
}

/* "0x68" or "104". A datasheet says the first; a script says the second. */
static bool parse_num(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') return false;

    bool hex = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    const char *p = hex ? s + 2 : s;
    if (*p == '\0') return false;

    uint32_t v = 0;
    for (; *p; p++) {
        uint32_t d;
        if (*p >= '0' && *p <= '9')              d = (uint32_t)(*p - '0');
        else if (hex && *p >= 'a' && *p <= 'f')  d = (uint32_t)(*p - 'a' + 10);
        else if (hex && *p >= 'A' && *p <= 'F')  d = (uint32_t)(*p - 'A' + 10);
        else return false;
        v = v * (hex ? 16u : 10u) + d;
    }
    *out = v;
    return true;
}

static void devpath(char *out, uint32_t addr)
{
    /* "/i2c0/" then the address in decimal -- IFM accepts either form, and
       decimal needs no hex formatter here. */
    m_devpath(out, "/i2c0/", addr);
}

static int scan(const rv9_mod_env_t *env)
{
    uint32_t found = 0;

    m_say(env, RV9_STDOUT, "scanning /i2c0\n");

    for (uint32_t a = 1; a <= 0x77u; a++) {
        char name[24];
        devpath(name, a);

        int p = env->open(name, RV9_MODE_READ);
        if (p < 0) continue;

        uint32_t present = 0;
        if (env->getstat(p, RV9_IFM_GS_PRESENT, &present) == 0 && present) {
            m_say(env, RV9_STDOUT, "  0x");
            hex2(env, (uint8_t)a);
            m_say(env, RV9_STDOUT, "\n");
            found++;
        }
        env->close(p);
    }

    if (found == 0) {
        /* The two failures look identical from here, so say both. */
        m_say(env, RV9_STDOUT, "nothing answered -- check the wiring, and "
                               "that both lines have pull-ups\n");
        return 1;
    }

    m_num(env, RV9_STDOUT, (int32_t)found);
    m_say(env, RV9_STDOUT, " device");
    if (found != 1) m_say(env, RV9_STDOUT, "s");
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return 2;

    i2c_statics_t *st = (i2c_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "i2c: static_size in build.conf is too small\n");
        return 3;
    }

    char word[16];
    word[0] = '\0';
    const char *rest = (env->arg != NULL) ? m_word(env->arg, word, sizeof(word))
                                          : "";

    if (word[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: i2c scan | i2c <addr> <reg> [count]\n");
        return 4;
    }

    if (m_eq(word, "scan")) return scan(env);

    uint32_t addr = 0;
    if (!parse_num(word, &addr)) {
        m_say(env, RV9_STDERR, "i2c: not an address\n");
        return 4;
    }

    char regtext[16];
    rest = m_word(rest, regtext, sizeof(regtext));
    uint32_t reg = 0;
    if (!parse_num(regtext, &reg)) {
        m_say(env, RV9_STDERR, "i2c: not a register\n");
        return 4;
    }

    char counttext[8];
    m_word(rest, counttext, sizeof(counttext));
    uint32_t count = 1;
    if (counttext[0] != '\0' && !parse_num(counttext, &count)) count = 1;
    if (count == 0 || count > MAX_READ) count = 1;

    char name[24];
    devpath(name, addr);

    int p = env->open(name, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDERR, name);
        m_say(env, RV9_STDERR, ": cannot open\n");
        return 5;
    }

    if (env->setstat(p, RV9_IFM_SS_REG, &reg) != 0) {
        m_say(env, RV9_STDERR, "i2c: this device takes no register\n");
        env->close(p);
        return 6;
    }

    int n = env->read(p, st->buf, count);
    env->close(p);

    if (n <= 0) {
        /* Silence on a two-wire bus is the ordinary failure, and it means
           the same thing whether the chip is absent, asleep or miswired. */
        m_say(env, RV9_STDERR, "i2c: no answer from 0x");
        hex2(env, (uint8_t)addr);
        m_say(env, RV9_STDERR, "\n");
        return 7;
    }

    for (int i = 0; i < n; i++) {
        if (i) m_say(env, RV9_STDOUT, " ");
        hex2(env, st->buf[i]);
    }
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
