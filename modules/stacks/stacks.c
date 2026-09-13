/*
 * stacks -- what each process was given, and what it has ever used.
 *
 * The stack is nearly the whole cost of a process: everything else comes
 * to about a kilobyte, and the default is eight. So a machine with fifty
 * kilobytes free holds six processes, almost none of which need anything
 * like that much.
 *
 * The used figure is measured rather than estimated: stacks are painted at
 * creation and scanned afterwards. A stack that runs out is caught -- the
 * kernel guards the floor and kills the thread -- but being caught is a
 * dead process, not a warning, so this is the number to cut a stack down
 * by. Spare of nearly zero means the next branch taken kills it.
 *
 * Below the living, every module that has run since boot and the deepest
 * any run of it went. Most commands end before anybody could look at them
 * live, so that second table is the one to size a command's stack from.
 */
#include "modlib.h"

#define MAX   16
#define PEAKS 64

typedef struct {
    rv9_sys_stack_t      s[MAX];
    rv9_sys_stack_peak_t p[PEAKS];
} stacks_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    stacks_t *st = (stacks_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int n = env->sysinfo(RV9_SYS_STACK, st->s, sizeof(st->s));
    if (n <= 0) { m_say(env, RV9_STDOUT, "no stack information\n"); return -3; }

    m_say(env, RV9_STDOUT, "pid  name             size   used   spare\n");

    uint32_t total = 0, used_total = 0;

    for (int i = 0; i < n; i++) {
        uint32_t size   = st->s[i].stack_size;
        uint32_t unused = st->s[i].stack_unused;
        uint32_t used   = (size > unused) ? size - unused : 0;

        total      += size;
        used_total += used;

        m_numpad(env, RV9_STDOUT, st->s[i].pid, 5);
        m_pad(env, RV9_STDOUT, st->s[i].name, 17);
        m_numpad(env, RV9_STDOUT, (int32_t)size, 7);
        m_numpad(env, RV9_STDOUT, (int32_t)used, 7);
        m_num(env, RV9_STDOUT, (int32_t)unused);
        m_say(env, RV9_STDOUT, "\n");
    }

    m_say(env, RV9_STDOUT, "\ngiven ");
    m_num(env, RV9_STDOUT, (int32_t)total);
    m_say(env, RV9_STDOUT, " bytes, used ");
    m_num(env, RV9_STDOUT, (int32_t)used_total);
    m_say(env, RV9_STDOUT, ", spare ");
    m_num(env, RV9_STDOUT, (int32_t)(total - used_total));
    m_say(env, RV9_STDOUT, "\n");

    /* A system that predates the record answers nothing, and that is not
       an error worth a word. */
    int m = env->sysinfo(RV9_SYS_STACK_PEAKS, st->p, sizeof(st->p));
    if (m <= 0) return 0;

    m_say(env, RV9_STDOUT, "\nsince boot       given   peak  runs\n");
    for (int i = 0; i < m && i < PEAKS; i++) {
        m_pad(env, RV9_STDOUT, st->p[i].name, 15);
        m_numpad(env, RV9_STDOUT, (int32_t)st->p[i].given, 7);
        m_numpad(env, RV9_STDOUT, (int32_t)st->p[i].peak, 7);
        m_numpad(env, RV9_STDOUT, (int32_t)st->p[i].runs, 6);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
