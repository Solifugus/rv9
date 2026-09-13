/*
 * free -- report memory and system counts.
 *
 * "Free" and "available" are different questions and both are printed.
 * The last few kilobytes are reserved for ESP-IDF's own internals, which
 * abort rather than fail when they cannot allocate; available is what RV-9
 * may actually spend, and it is the number that decides whether the next
 * process starts.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 5) return -1;

    rv9_sys_mem_t m;
    if (env->sysinfo(RV9_SYS_MEM, &m, sizeof(m)) < 1) return -2;

    m_say(env, RV9_STDOUT, "heap free      ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_free);
    m_say(env, RV9_STDOUT, "\navailable      ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_available);

    /* The first is what a program you type may still take; the second is
       held back so a control loop can be admitted, and a failsafe applied,
       after the first has run out. */
    m_say(env, RV9_STDOUT, "\n  for programs ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_general);
    m_say(env, RV9_STDOUT, "\n  rt reserve   ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_rt_reserve);
    m_say(env, RV9_STDOUT, "\nreserved       ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_floor);
    m_say(env, RV9_STDOUT, "\nrefused        ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_refusals);
    m_say(env, RV9_STDOUT, "\nlow water      ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_low_water);
    m_say(env, RV9_STDOUT, "\nexecutable     ");
    m_num(env, RV9_STDOUT, (int32_t)m.heap_exec_free);
    m_say(env, RV9_STDOUT, "\nmodules        ");
    m_num(env, RV9_STDOUT, (int32_t)m.module_count);
    m_say(env, RV9_STDOUT, "\nprocesses      ");
    m_num(env, RV9_STDOUT, (int32_t)m.proc_count);
    m_say(env, RV9_STDOUT, "\n");

    return 0;
}
