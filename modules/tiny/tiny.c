/*
 * tiny -- how little stack a process can actually have.
 *
 * Not a demonstration of anything except the number, which matters: every
 * process costs its stack whether it uses it or not, and at the eight
 * kilobyte default a board with fifty free can hold six of them. A
 * language whose programs are processes needs to know where the floor is.
 *
 * It does what an ordinary small program does -- formats something and
 * writes it through the I/O manager -- because the stack that matters is
 * the one a real call chain uses, not the one an empty function needs.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    char buf[64];
    uint32_t n = 0;
    const char *msg = "tiny: ran on a small stack, arg ";
    while (msg[n] && n < sizeof(buf) - 8) { buf[n] = msg[n]; n++; }
    buf[n++] = env->arg && env->arg[0] ? env->arg[0] : '-';
    buf[n++] = '\n';

    env->write(RV9_STDOUT, buf, n);
    return 0;
}
