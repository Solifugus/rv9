/*
 * rt -- run a module in the real-time class.  rt control [period_us]
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 10) return -1;
    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: rt <module> [period_us]\n");
        return -2;
    }

    char name[32];
    uint32_t i = 0;
    while (env->arg[i] && env->arg[i] != ' ' && i < sizeof(name) - 1) {
        name[i] = env->arg[i]; i++;
    }
    name[i] = '\0';

    const char *rest = env->arg + i;
    while (*rest == ' ') rest++;

    uint32_t period = 1000;
    if (*rest) {
        uint32_t v = 0;
        for (uint32_t j = 0; rest[j] >= '0' && rest[j] <= '9'; j++) {
            v = v * 10 + (uint32_t)(rest[j] - '0');
        }
        if (v >= 100) period = v;
    }

    int pid = env->fork_rt(name, period, rest);
    if (pid < 0) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": cannot start as real-time\n");
        return -3;
    }

    m_say(env, RV9_STDOUT, "rt: started ");
    m_say(env, RV9_STDOUT, name);
    m_say(env, RV9_STDOUT, " as pid ");
    m_num(env, RV9_STDOUT, pid);
    m_say(env, RV9_STDOUT, "\n");

    int status = 0;
    int w = env->wait(pid, &status, 60000);
    if (w < 0) {
        m_say(env, RV9_STDOUT, "rt: wait failed\n");
        return -6;
    }
    return status;
}
