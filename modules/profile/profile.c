/*
 * profile -- what this machine offers a program, as JSON.
 *
 * RV-9's target profile has two halves. What every RV-9 build offers --
 * the ABI, the manifest, which calls are real-time safe -- is generated
 * from the sources into docs/target/rv9-profile.json. What *this board*
 * offers is only known here: the devices it has, what each is built from
 * and whether it holds its state, how many real-time loops it can admit,
 * and where its real-time priorities sit against the radio.
 *
 * A toolchain captures it the way anything else is captured from RV-9:
 *
 *     printf 'profile\nexit\n' | ssh board          over the network
 *     profile > /r0/profile.json                    onto a volume
 */
#include "modlib.h"

#define MAX_DEVICES 24

typedef struct {
    rv9_sys_limits_t limits;
    rv9_sys_device_t devs[MAX_DEVICES];
} profile_statics_t;

static void key(const rv9_mod_env_t *env, const char *indent, const char *k)
{
    m_say(env, RV9_STDOUT, indent);
    m_say(env, RV9_STDOUT, "\"");
    m_say(env, RV9_STDOUT, k);
    m_say(env, RV9_STDOUT, "\": ");
}

static void num(const rv9_mod_env_t *env, const char *indent, const char *k,
                uint32_t v, bool last)
{
    key(env, indent, k);
    m_num(env, RV9_STDOUT, (int32_t)v);
    m_say(env, RV9_STDOUT, last ? "\n" : ",\n");
}

static void str(const rv9_mod_env_t *env, const char *k, const char *v)
{
    m_say(env, RV9_STDOUT, "\"");
    m_say(env, RV9_STDOUT, k);
    m_say(env, RV9_STDOUT, "\": \"");
    m_say(env, RV9_STDOUT, v);
    m_say(env, RV9_STDOUT, "\"");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;

    profile_statics_t *st = (profile_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->sysinfo(RV9_SYS_LIMITS, &st->limits, sizeof(st->limits)) < 1) {
        m_say(env, RV9_STDOUT, "profile: this system does not report its "
                               "limits\n");
        return -3;
    }
    int n = env->sysinfo(RV9_SYS_DEVICES, st->devs, sizeof(st->devs));
    if (n < 0) n = 0;
    if (n > MAX_DEVICES) n = MAX_DEVICES;

    const rv9_sys_limits_t *l = &st->limits;

    m_say(env, RV9_STDOUT, "{\n  \"profile\": \"rv9-board\",\n");
    num(env, "  ", "module_abi", l->module_abi, false);

    m_say(env, RV9_STDOUT, "  \"realtime\": {\n");
    num(env, "    ", "slots", l->rt_slots, false);
    num(env, "    ", "utilisation_ceiling_permille",
        l->rt_util_ceiling_permille, false);
    num(env, "    ", "runaway_ms", l->rt_runaway_ms, false);
    num(env, "    ", "watchdog_us", l->rt_watchdog_us, false);
    m_say(env, RV9_STDOUT, "    \"host_priority\": {\n");
    num(env, "      ", "urgent", l->prio_urgent, false);
    num(env, "      ", "routine", l->prio_routine, false);
    num(env, "      ", "radio", l->prio_radio, false);
    num(env, "      ", "kernel", l->prio_kernel, true);
    m_say(env, RV9_STDOUT, "    }\n  },\n");

    m_say(env, RV9_STDOUT, "  \"memory\": {\n");
    num(env, "    ", "heap_floor", l->heap_floor, true);
    m_say(env, RV9_STDOUT, "  },\n");

    m_say(env, RV9_STDOUT, "  \"processes\": {\n");
    num(env, "    ", "history", l->proc_history, false);
    num(env, "    ", "history_max", l->proc_history_max, false);
    num(env, "    ", "max_paths", l->max_paths, true);
    m_say(env, RV9_STDOUT, "  },\n");

    m_say(env, RV9_STDOUT, "  \"devices\": [\n");
    for (int i = 0; i < n; i++) {
        const rv9_sys_device_t *d = &st->devs[i];
        m_say(env, RV9_STDOUT, "    { ");
        str(env, "name", d->name);
        m_say(env, RV9_STDOUT, ", ");
        str(env, "filemgr", d->filemgr);
        m_say(env, RV9_STDOUT, ", ");
        str(env, "driver", d->driver);
        m_say(env, RV9_STDOUT, d->retains ? ", \"retains\": true"
                                          : ", \"retains\": false");
        m_say(env, RV9_STDOUT, d->sessions ? ", \"sessions\": true }"
                                           : ", \"sessions\": false }");
        m_say(env, RV9_STDOUT, (i + 1 < n) ? ",\n" : "\n");
    }
    m_say(env, RV9_STDOUT, "  ]\n}\n");
    return 0;
}
