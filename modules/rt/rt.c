/*
 * rt -- ask for a module to be run in the real-time class.
 *
 *   rt                    what the machine has already promised
 *   rt <module>           at the rate the module says it needs
 *   rt <module> <period>  at a rate an operator insists on instead
 *
 * "Ask" rather than "run": a real-time program is admitted, not started.
 * RV-9 checks the declaration against itself and against what is already
 * promised, and the interesting output of this command is often a refusal.
 */
#include "modlib.h"

/*
 * Parts per thousand, printed as a percentage with one decimal, because
 * a control loop's duty is frequently under one percent and "0%" is not
 * a useful thing to tell somebody.
 */
static void say_permille(const rv9_mod_env_t *env, uint32_t pm)
{
    m_num(env, RV9_STDOUT, (int32_t)(pm / 10));
    m_say(env, RV9_STDOUT, ".");
    m_num(env, RV9_STDOUT, (int32_t)(pm % 10));
    m_say(env, RV9_STDOUT, "%");
}

static void report(const rv9_mod_env_t *env)
{
    rv9_sys_admit_t a;
    if (env->sysinfo(RV9_SYS_ADMIT, &a, sizeof(a)) < 1) {
        m_say(env, RV9_STDOUT, "no admission information\n");
        return;
    }

    m_say(env, RV9_STDOUT, "real-time promised  ");
    say_permille(env, a.used_permille);
    m_say(env, RV9_STDOUT, " of ");
    say_permille(env, a.ceiling_permille);
    m_say(env, RV9_STDOUT, "\nslots               ");
    m_num(env, RV9_STDOUT, (int32_t)a.slots_used);
    m_say(env, RV9_STDOUT, " of ");
    m_num(env, RV9_STDOUT, (int32_t)a.slots_total);

    m_say(env, RV9_STDOUT, "\ndeclared            ");
    m_num(env, RV9_STDOUT, (int32_t)a.declared);
    m_say(env, RV9_STDOUT, "\nmeasured            ");
    m_num(env, RV9_STDOUT, (int32_t)a.measured);
    m_say(env, RV9_STDOUT, "\nunaccounted         ");
    m_num(env, RV9_STDOUT, (int32_t)a.unaccounted);
    m_say(env, RV9_STDOUT, "\n");

    /* Say what the number is worth. A total standing on measurements is
       a floor, not a promise, and reporting it as though it were a
       promise is the failure this whole exercise is against. */
    if (a.measured > 0 || a.unaccounted > 0) {
        m_say(env, RV9_STDOUT,
              "\nthat total is a floor: some work has not said what it "
              "costs\n");
    }

    /*
     * Each loop: where it was placed, what the analysis promises, and what
     * it has actually done. The last two side by side are the point -- a
     * bound that measurement keeps approaching is a declaration to look at.
     */
    rv9_sys_rt_t recs[4];
    int n = env->sysinfo(RV9_SYS_RT, recs, sizeof(recs));
    if (n <= 0) return;
    if (n > 4) n = 4;

    m_say(env, RV9_STDOUT,
          "\nslot  period  deadline  runs     bound  worst  misses\n");
    for (int i = 0; i < n; i++) {
        const rv9_sys_rt_t *r = &recs[i];
        m_numpad(env, RV9_STDOUT, r->index, 6);
        m_numpad(env, RV9_STDOUT, (int32_t)r->period_us, 8);
        m_numpad(env, RV9_STDOUT, (int32_t)r->deadline_us, 10);
        m_pad(env, RV9_STDOUT, r->urgent ? "urgent" : "routine", 9);
        if (r->bound_us) {
            m_numpad(env, RV9_STDOUT, (int32_t)r->bound_us, 7);
        } else {
            m_pad(env, RV9_STDOUT, "-", 7);
        }
        m_numpad(env, RV9_STDOUT, (int32_t)r->max_response_us, 7);
        m_num(env, RV9_STDOUT, (int32_t)r->deadline_misses);
        m_say(env, RV9_STDOUT, "\n");
    }
}

/* Why the machine said no. Each of these is actionable, which is why
   they are separate codes and not one refusal. */
static void refusal(const rv9_mod_env_t *env, const char *name, int pid)
{
    m_say(env, RV9_STDOUT, name);

    if (pid == -RV9_PE_NOSLOT) {
        m_say(env, RV9_STDOUT, ": no real-time slot free -- stop one first\n");
    } else if (pid == -RV9_PE_CONTRACT) {
        m_say(env, RV9_STDOUT, ": its declaration contradicts itself "
                               "(see the log)\n");
    } else if (pid == -RV9_PE_UTILISATION) {
        m_say(env, RV9_STDOUT, ": the CPU is already promised\n");
        report(env);
    } else if (pid == -RV9_PE_NOMEM) {
        m_say(env, RV9_STDOUT, ": not enough memory can be guaranteed it\n");
    } else if (pid == -RV9_PE_NOTFOUND) {
        m_say(env, RV9_STDOUT, ": no such module\n");
    } else if (pid == -RV9_PE_BUSY) {
        m_say(env, RV9_STDOUT, ": a device it needs alone is owned "
                               "(see 'owns')\n");
    } else if (pid == -RV9_PE_NODEV) {
        m_say(env, RV9_STDOUT, ": this machine has no such device\n");
    } else if (pid == -RV9_PE_NOPUB) {
        m_say(env, RV9_STDOUT, ": it watches a publication nothing on this "
                               "machine provides\n");
    } else if (pid == -RV9_PE_BUDGET) {
        m_say(env, RV9_STDOUT, ": over the memory budget of whatever is "
                               "starting it (see 'budgets')\n");
    } else if (pid == -RV9_PE_UNSCHEDULABLE) {
        /* Not the utilisation refusal: the CPU has room, the deadlines do
           not. The log names which loop would be late, and by how much. */
        m_say(env, RV9_STDOUT, ": with it running, some loop would miss its "
                               "deadline (see the log)\n");
        report(env);
    } else {
        m_say(env, RV9_STDOUT, ": cannot start as real-time\n");
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 10) return -1;

    /* No argument is a question, not a mistake. */
    if (env->arg == NULL || env->arg[0] == '\0') {
        report(env);
        m_say(env, RV9_STDOUT, "\nusage: rt <module> [period_us]\n");
        return 0;
    }

    char name[32];
    uint32_t i = 0;
    while (env->arg[i] && env->arg[i] != ' ' && i < sizeof(name) - 1) {
        name[i] = env->arg[i]; i++;
    }
    name[i] = '\0';

    const char *rest = env->arg + i;
    while (*rest == ' ') rest++;

    /* Zero means "whatever the module says it needs". A period typed at a
       shell is an override, not a default: the rate belongs to the control
       law, and the module's manifest is where it is written down. */
    uint32_t period = 0;
    if (*rest) {
        uint32_t v = 0;
        for (uint32_t j = 0; rest[j] >= '0' && rest[j] <= '9'; j++) {
            v = v * 10 + (uint32_t)(rest[j] - '0');
        }
        if (v >= 100) period = v;
    }

    int pid = env->fork_rt(name, period, rest);
    if (pid < 0) {
        refusal(env, name, pid);
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

    /* In the order it happened: the devices were parked before the
       reason was published, so saying so second is saying it true. */
    if (status == -RV9_PE_DEADLINE) {
        m_say(env, RV9_STDOUT, "rt: ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, " missed its deadline: failsafes applied, "
                               "stopped, not released again\n");
    } else if (status == -RV9_PE_RUNAWAY) {
        m_say(env, RV9_STDOUT, "rt: ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, " stopped waiting for its releases: stopped "
                               "from outside, failsafes applied\n");
    } else if (status == -RV9_PE_KILLED) {
        m_say(env, RV9_STDOUT, "rt: ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, " was killed\n");
    }

    /*
     * Said, so not passed on. Handing the child's -RV9_PE_DEADLINE up as
     * this program's status made the shell report that `rt` had missed a
     * deadline -- true of the child, false of rt, and printed right under
     * the line that said which.
     */
    if (status == -RV9_PE_DEADLINE || status == -RV9_PE_KILLED ||
        status == -RV9_PE_RUNAWAY) {
        return 1;
    }
    return status;
}
