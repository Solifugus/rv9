/*
 * boots -- how this machine has been keeping.
 *
 *   rv9> boots
 *   boot 41, up 2h 5m, normal
 *   boot  started by              mode    up        crash at
 *     38  power on                normal  3d 4h     -
 *     39  a panic                 normal  12 s      0x4200a1c4
 *     40  a panic                 normal  9 s       0x4200a1c4
 *     41  a panic                 SAFE    2h 5m     0x4200a1c4
 *   last crash: Load access fault, pc 0x4200a1c4 ra 0x4200a19e cause 5 value 0x0
 *
 * Each boot is said by how it started, which is how the run before it
 * ended; `up` is how long it ran (written hourly, so a run cut short shows
 * its last hour). Three bad ends in a row, none of them up five minutes,
 * and a board is in safe mode (design section 82; `safe`). The crash's
 * address is the one to give addr2line against the firmware's ELF.
 */
#include "modlib.h"

typedef struct {
    rv9_sys_health_t h;
    char hex[11];
} boots_statics_t;

static const char *started(uint8_t r)
{
    switch (r) {
    case RV9_RESET_POWERON:  return "power on";
    case RV9_RESET_EXT:      return "the reset pin";
    case RV9_RESET_SW:       return "asked to";
    case RV9_RESET_PANIC:    return "a panic";
    case RV9_RESET_INT_WDT:  return "interrupt watchdog";
    case RV9_RESET_TASK_WDT: return "task watchdog";
    case RV9_RESET_WDT:      return "a watchdog";
    case RV9_RESET_BROWNOUT: return "a brownout";
    case RV9_RESET_USB:      return "the USB port";
    case RV9_RESET_JTAG:     return "JTAG";
    default:                 return "unknown";
    }
}

static void hex(boots_statics_t *st, uint32_t v)
{
    st->hex[0] = '0';
    st->hex[1] = 'x';
    for (int i = 0; i < 8; i++) st->hex[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    st->hex[10] = '\0';
}

/* "3d 4h", "2h 5m", "12 s" */
static void span(const rv9_mod_env_t *env, uint32_t s, uint32_t width)
{
    char out[16];
    uint32_t n = 0, a, b;
    const char *ua, *ub;
    if (s >= 86400)    { a = s / 86400; b = s % 86400 / 3600; ua = "d "; ub = "h"; }
    else if (s >= 3600) { a = s / 3600; b = s % 3600 / 60;    ua = "h "; ub = "m"; }
    else if (s >= 60)   { a = s / 60;   b = s % 60;           ua = "m "; ub = "s"; }
    else                { a = s;        b = 0;                ua = " s"; ub = 0;   }
    char d[10];
    int c = 0;
    do { d[c++] = (char)('0' + a % 10); a /= 10; } while (a);
    while (c) out[n++] = d[--c];
    for (const char *u = ua; *u; u++) out[n++] = *u;
    if (ub) {
        do { d[c++] = (char)('0' + b % 10); b /= 10; } while (b);
        while (c) out[n++] = d[--c];
        for (const char *u = ub; *u; u++) out[n++] = *u;
    }
    out[n] = '\0';
    m_pad(env, RV9_STDOUT, out, width);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 20) return 2;   /* RV9_SYS_HEALTH */

    boots_statics_t *st = (boots_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    st->h.op = RV9_HEALTH_ASK;
    if (env->sysinfo(RV9_SYS_HEALTH, &st->h, sizeof(st->h)) != 1) {
        m_say(env, RV9_STDERR, "boots: this firmware keeps no record\n");
        return 1;
    }

    m_say(env, RV9_STDOUT, "boot ");
    m_num(env, RV9_STDOUT, (int32_t)st->h.boot);
    m_say(env, RV9_STDOUT, ", up ");
    span(env, st->h.up_s, 0);
    m_say(env, RV9_STDOUT, st->h.safe ? "-- SAFE MODE" : "-- normal");
    if (st->h.trial) m_say(env, RV9_STDOUT, " (a try out of safe mode)");
    m_say(env, RV9_STDOUT, "\n");

    m_say(env, RV9_STDOUT, "boot  started by              mode    up        crash at\n");
    for (uint32_t i = 0; i < st->h.n && i < 16; i++) {
        const rv9_sys_boot_t *b = &st->h.boots[i];
        m_numpad(env, RV9_STDOUT, (int32_t)b->boot, 6);
        m_pad(env, RV9_STDOUT, started(b->reason), 24);
        m_pad(env, RV9_STDOUT, b->safe ? "SAFE" : b->trial ? "trial" : "normal", 8);
        span(env, b->up_s, 10);
        if (b->pc) { hex(st, b->pc); m_say(env, RV9_STDOUT, st->hex); }
        else m_say(env, RV9_STDOUT, "-");
        m_say(env, RV9_STDOUT, "\n");
    }

    if (st->h.panic[0]) {
        st->h.panic[47] = '\0';
        m_say(env, RV9_STDOUT, "last crash: ");
        m_say(env, RV9_STDOUT, st->h.panic);
        hex(st, st->h.panic_pc);
        m_say(env, RV9_STDOUT, ", pc ");
        m_say(env, RV9_STDOUT, st->hex);
        hex(st, st->h.panic_ra);
        m_say(env, RV9_STDOUT, " ra ");
        m_say(env, RV9_STDOUT, st->hex);
        m_say(env, RV9_STDOUT, " cause ");
        m_num(env, RV9_STDOUT, (int32_t)st->h.panic_cause);
        hex(st, st->h.panic_tval);
        m_say(env, RV9_STDOUT, " value ");
        m_say(env, RV9_STDOUT, st->hex);
        m_say(env, RV9_STDOUT, "  (log -p)\n");
    }
    if (st->h.fails && !st->h.safe) {
        m_num(env, RV9_STDOUT, st->h.fails);
        m_say(env, RV9_STDOUT, " bad end(s) in a row; three and the next boot is in safe mode\n");
    }
    return 0;
}
