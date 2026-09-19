/*
 * dump -- what the bytes actually are.
 *
 *   dump /f0/prog.mod | first 4
 *   dump < /r0/notes
 *
 * Sixteen bytes a line, offset, hex, then the printable characters. On a
 * board this earns its place: a module header, a sector, a packet that
 * arrived wrong -- none of them are text, and `cat` on them tells you
 * nothing except that your terminal has a bell.
 *
 * Reads standard input when given no name, like every other filter here.
 */
#include "modlib.h"

#define WIDTH 16

typedef struct { uint8_t buf[WIDTH]; } dump_statics_t;

/* Arithmetic rather than a lookup table: a `static const` array inside a
   function is a variable at a fixed address, and a module that refers to
   one is no longer position independent. The linker says so, which is the
   rule earning its keep. */
static char nybble(uint8_t v)
{
    v &= 0xF;
    return (char)((v < 10) ? ('0' + v) : ('a' + (v - 10)));
}

static void hex2(const rv9_mod_env_t *env, uint8_t v)
{
    char out[3];
    out[0] = nybble((uint8_t)(v >> 4));
    out[1] = nybble(v);
    out[2] = '\0';
    m_say(env, RV9_STDOUT, out);
}

static void hex4(const rv9_mod_env_t *env, uint32_t v)
{
    hex2(env, (uint8_t)(v >> 8));
    hex2(env, (uint8_t)v);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    dump_statics_t *st = (dump_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "dump: static_size in build.conf is too small\n");
        return -2;
    }

    bool named = (env->arg != NULL && env->arg[0] != '\0' &&
                  env->arg[0] != ' ');
    int p = RV9_STDIN;

    if (named) {
        char name[48];
        m_word(env->arg, name, sizeof(name));
        p = env->open(name, RV9_MODE_READ);
        if (p < 0) {
            m_say(env, RV9_STDERR, name);
            m_say(env, RV9_STDERR, ": cannot open\n");
            return -3;
        }
    }

    uint32_t offset = 0;
    for (;;) {
        /* Fill a whole line before printing one, so a slow source does not
           produce ragged output. */
        int have = 0;
        while (have < WIDTH) {
            int n = env->read(p, st->buf + have, (uint32_t)(WIDTH - have));
            if (n <= 0) break;
            have += n;
        }
        if (have == 0) break;

        hex4(env, offset);
        m_say(env, RV9_STDOUT, "  ");

        for (int i = 0; i < WIDTH; i++) {
            if (i < have) hex2(env, st->buf[i]);
            else          m_say(env, RV9_STDOUT, "  ");
            m_say(env, RV9_STDOUT, " ");
            if (i == 7) m_say(env, RV9_STDOUT, " ");
        }

        m_say(env, RV9_STDOUT, " ");
        char ch[2] = { 0, 0 };
        for (int i = 0; i < have; i++) {
            uint8_t c = st->buf[i];
            ch[0] = (c >= 32 && c < 127) ? (char)c : '.';
            m_say(env, RV9_STDOUT, ch);
        }
        m_say(env, RV9_STDOUT, "\n");

        offset += (uint32_t)have;
        if (have < WIDTH) break;
    }

    if (named) env->close(p);
    return 0;
}
