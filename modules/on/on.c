/*
 * on -- a command run on another machine, as if here.
 *
 *   rv9> on p4 listen
 *   rv9> cat words | on p4 sort > /sd0/sorted
 *   rv9> listen                     (on a C5: the kernel forks `on p4 listen`)
 *
 * Its standard output and error come back as the command writes them, and
 * its status is this process's, so a shell, a pipeline or a program waiting
 * on it cannot tell where it ran. Killing this hangs up, and the peer stops
 * the command.
 *
 * The peer is one /f0/peers names, and it runs the command only if this
 * machine is one of its /f0/callers: /peer/<name>/run proves who this is
 * with the key `ident` names (peerfm.c, design section 76). What it runs is
 * its own, found by its own path.
 *
 * The kernel starts this in place of a command the module path found on a
 * peer and this machine cannot run, for want of the CPU or a device: the
 * code moves when it can, and the work when it must.
 *
 * Standard input is sent on from a pipe or a file. From the terminal it is
 * not -- the command reads an end at once -- since a terminal cannot say
 * whether a key is waiting, and a command in the background would
 * otherwise take the shell's keystrokes.
 */
#include "modlib.h"
#include "run.h"

#define RUN_PATH_MAX 40

typedef struct {
    char     peer[16];
    char     cmd[32];
    char     path[RUN_PATH_MAX];
    uint8_t  from[RUN_FRAME_MAX + 3 + 128];
    uint32_t have;
    uint8_t  in[256];
    int32_t  status;
} on_statics_t;

static void err(const rv9_mod_env_t *env, const char *s) { m_say(env, RV9_STDERR, s); }

static void cat(char *dst, uint32_t cap, const char *a, const char *b, const char *c)
{
    uint32_t i = 0;
    const char *parts[3] = { a, b, c };
    for (int k = 0; k < 3; k++) {
        for (const char *s = parts[k]; s && *s && i < cap - 1; s++) dst[i++] = *s;
    }
    dst[i] = '\0';
}

/* Why the peer did not start it, in words. */
static void refused(const rv9_mod_env_t *env, on_statics_t *st, int32_t why)
{
    err(env, st->cmd);
    err(env, " on ");
    err(env, st->peer);
    switch (-why) {
    case RV9_PE_NOTFOUND:
    case RV9_PE_MODULE:   err(env, ": no such command there, or none it can load "
                               "(see its log)\n"); break;
    case RV9_PE_NODEV:    err(env, ": it needs a device that machine lacks\n"); break;
    case RV9_PE_ISA:      err(env, ": it needs instructions that CPU lacks\n"); break;
    case RV9_PE_BUSY:     err(env, ": a device it needs alone is in use there\n"); break;
    case RV9_PE_NOMEM:
    case RV9_PE_BUDGET:   err(env, ": no room for it there\n"); break;
    default:
        err(env, ": refused there, code ");
        m_num(env, RV9_STDERR, -why);
        err(env, " (see its log)\n");
    }
}

/* 1 once the command has ended, with its status; -1 once it never will. */
static int take(const rv9_mod_env_t *env, void *ctx, char kind, const uint8_t *data,
                uint32_t len)
{
    on_statics_t *st = (on_statics_t *)ctx;
    switch (kind) {
    case RUN_OUT:
        env->write(RV9_STDOUT, data, len);
        return 0;
    case RUN_ERR:
        env->write(RV9_STDERR, data, len);
        return 0;
    case RUN_EXIT:
        if (len < 8) return -1;
        st->status = run_get32(data);
        if (run_get32(data + 4) != RV9_FAULT_NONE) {
            err(env, st->cmd);
            err(env, " on ");
            err(env, st->peer);
            err(env, run_get32(data + 4) == RV9_FAULT_KILLED ? ": killed\n"
                     : ": stopped by the scheduler there (see its log)\n");
            if (st->status == 0) st->status = -RV9_PE_FAULT;
        }
        return 1;
    case RUN_REFUSED:
        if (len < 4) return -1;
        refused(env, st, run_get32(data));
        st->status = run_get32(data);
        return 1;
    default:
        return -1;
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 18) return 2;   /* /peer/<name>/run */

    on_statics_t *st = (on_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    /* "p4 listen" or "p4 sort -r": the peer, then what to ask it for. */
    const char *rest = m_word(env->arg ? env->arg : "", st->peer, sizeof(st->peer));
    while (*rest == ' ') rest++;
    const char *ask = rest;
    rest = m_word(rest, st->cmd, sizeof(st->cmd));
    if (st->peer[0] == '\0' || st->cmd[0] == '\0') {
        err(env, "usage: on <peer> <command> [arg]\n");
        return 2;
    }
    (void)rest;

    cat(st->path, sizeof(st->path), "/peer/", st->peer, "/run");
    int c = env->open(st->path, RV9_MODE_RW);
    if (c < 0) {
        err(env, st->cmd);
        err(env, " on ");
        err(env, st->peer);
        err(env, c == -RV9_IOE_DENIED   ? ": that machine does not run commands for this "
                                          "one (`ident` here, and its /f0/callers)\n"
               : c == -RV9_IOE_NOTFOUND ? ": no such peer in /f0/peers\n"
               : c == -RV9_IOE_UNSUPPORTED ? ": this machine has no key to say who it is\n"
               : ": cannot reach its execd (see the log)\n");
        return 3;
    }
    if (run_send(env, c, RUN_ASK, ask, m_len(ask)) < 0) {
        env->close(c);
        return 3;
    }

    /* Standard input: a pipe says when there is some, and a file always
       has; a terminal cannot say, so the command gets an end at once. */
    uint32_t ready = 0;
    uint64_t size = 0;
    bool in_poll = env->getstat(RV9_STDIN, RV9_GS_READY, &ready) >= 0;
    bool in_open = in_poll || env->getstat(RV9_STDIN, RV9_GS_SIZE, &size) >= 0;
    if (!in_open) run_send(env, c, RUN_EOF, 0, 0);

    uint32_t one = 1;
    env->setstat(c, RUN_SS_NOWAIT, &one);
    st->have = 0;
    st->status = 0;
    int done = 0;
    while (done == 0) {
        bool busy = false;

        int n = env->read(c, st->from + st->have, sizeof(st->from) - st->have);
        if (n > 0) {
            st->have += (uint32_t)n;
            done = run_frames(env, st, st->from, &st->have, take);
            busy = true;
        } else if (n != -RV9_IOE_WOULDBLOCK) {
            done = -1;
        }
        if (done) break;

        if (in_open && (!in_poll ||
                        (env->getstat(RV9_STDIN, RV9_GS_READY, &ready) >= 0 && ready))) {
            int got = env->read(RV9_STDIN, st->in, sizeof(st->in));
            if (got > 0) {
                run_send(env, c, RUN_IN, st->in, (uint32_t)got);
            } else {
                run_send(env, c, RUN_EOF, 0, 0);
                in_open = false;
            }
            busy = true;
        }

        if (!busy) env->sleep_ms(5);
    }
    env->close(c);

    if (done < 0) {
        err(env, st->cmd);
        err(env, " on ");
        err(env, st->peer);
        err(env, ": the connection ended before it did\n");
        return 3;
    }
    return st->status;
}
