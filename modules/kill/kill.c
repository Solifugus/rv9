/*
 * kill -- stop a process.
 *
 *   kill <pid>      ask it to stop, and stop it if it has not in two seconds
 *   kill -f <pid>   stop it now, without asking
 *
 * Asking comes first because asking is what lets a program clean up: a
 * process that sees RV9_SIG_STOP closes its own paths, puts its own
 * terminal back, says goodbye. Being killed does none of that. RV-9 still
 * closes the paths and applies the failsafes, because those are RV-9's to
 * apply however a process ends, but anything the program meant to do on
 * the way out does not happen.
 *
 * "Stop it now" is not "stop it wherever it is". RV-9 stops an ordinary
 * process at a moment it holds no lock, and a real-time one between
 * activations -- because a process stopped while holding a lock takes the
 * lock with it, and the next thing to want that lock waits forever. Kill
 * the diagnostic, hang the control loop. So a kill can come back "not
 * yet", and says so.
 */
#include "modlib.h"

#define GRACE_MS 2000

static void usage(const rv9_mod_env_t *env)
{
    m_say(env, RV9_STDOUT, "usage: kill [-f] <pid>\n"
                           "  asks first; -f does not ask\n");
}

static void ended(const rv9_mod_env_t *env, int pid, int status)
{
    m_say(env, RV9_STDOUT, "pid ");
    m_num(env, RV9_STDOUT, pid);
    if (status == -RV9_PE_KILLED) {
        m_say(env, RV9_STDOUT, " killed\n");
    } else {
        m_say(env, RV9_STDOUT, " stopped when asked, status ");
        m_num(env, RV9_STDOUT, status);
        m_say(env, RV9_STDOUT, "\n");
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL) return -1;
    if (env->abi_version < 13 || env->kill == NULL) {
        m_say(env, RV9_STDOUT, "kill: this system cannot stop processes\n");
        return -1;
    }

    const char *a = env->arg;
    if (a == NULL) { usage(env); return 1; }

    while (*a == ' ') a++;
    bool force = false;
    if (a[0] == '-' && a[1] == 'f') {
        force = true;
        a += 2;
        while (*a == ' ') a++;
    }

    const char *end = a;
    int pid = (int)m_num_parse(a, &end);
    if (pid <= 0 || end == a) { usage(env); return 1; }

    int status = 0;

    if (!force) {
        if (env->signal(pid, RV9_SIG_STOP) < 0) {
            m_say(env, RV9_STDOUT, "kill: no process ");
            m_num(env, RV9_STDOUT, pid);
            m_say(env, RV9_STDOUT, "\n");
            return 1;
        }
        if (env->wait(pid, &status, GRACE_MS) == 0) {
            ended(env, pid, status);
            return 0;
        }
        m_say(env, RV9_STDOUT, "pid ");
        m_num(env, RV9_STDOUT, pid);
        m_say(env, RV9_STDOUT, " did not stop when asked; stopping it\n");
    }

    int r = env->kill(pid);

    if (r == 0) {
        /* Already ended, so this does not wait; it collects the status. */
        env->wait(pid, &status, 0);
        ended(env, pid, status);
        return 0;
    }

    m_say(env, RV9_STDOUT, "kill: pid ");
    m_num(env, RV9_STDOUT, pid);
    if (r == -RV9_PE_NOTFOUND) {
        m_say(env, RV9_STDOUT, ": no such process\n");
    } else if (r == -RV9_PE_TIMEOUT) {
        /* Not a refusal. Say which kind of not-yet it is likely to be. */
        m_say(env, RV9_STDOUT, ": no safe moment to stop it yet -- a "
                               "real-time process stops at its next "
                               "release; an ordinary one was holding a "
                               "lock, so try again\n");
    } else {
        m_say(env, RV9_STDOUT, ": cannot be stopped from outside "
                               "(see the log)\n");
    }
    return 1;
}
