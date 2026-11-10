/*
 * execd -- commands run here for the peers allowed to ask.
 *
 * The other end of `on` (design section 76). A peer that opens
 * /peer/<this>/run and proves it is one of /f0/callers gets a connection;
 * execd hands it to a session of its own, as rshd hands a connection to a
 * shell, and listens again. The session reads which command, forks it with
 * three pipes for its standard paths, and carries what goes through them
 * to and from the caller in frames (run.h), and its status at the end.
 *
 * Who may ask is /f0/callers, one peer a line, a name and the id `ident`
 * prints on that peer:
 *
 *     c5  1f0c9a4e5b7d...      (32 hex digits)
 *
 * Started by init when the file exists; a board nobody may run things on
 * does not listen for them. Proving who connected is peerfm's, in the
 * kernel, where this machine's key is; here there is only the plumbing.
 *
 * A caller that hangs up, or goes silent without a word (switched off,
 * reset: TCP's keep-alive notices in about five seconds, peerfm.c), has
 * its command killed, so whatever the command declared as failsafes is
 * applied: a motor a C3 was driving for the P4 stops when the P4 does.
 *
 * What it runs is found here, by this machine's own path, and what that
 * starts runs here or nowhere: a process working for a peer is never moved
 * to a peer in turn, so two machines that each run the other's commands
 * cannot pass one back and forth.
 */
#include "modlib.h"
#include "run.h"

#define LISTEN_PATH "/peer/listen"
#define SAVE_IN     5
#define SAVE_ERR    7
#define ASK_MAX     96

typedef struct {
    uint32_t seq;               /* sessions started: pipe names */
    char     who[16];
    char     ask[ASK_MAX];
    char     pipe[3][16];       /* in, out, err */
    uint8_t  from[RUN_FRAME_MAX + 3 + 128];
    uint32_t have;
    uint8_t  buf[RUN_FRAME_MAX];
    int      in_w;              /* the command's stdin, our end; -1 once ended */
    int      pid;
} execd_statics_t;

static void log_say(const rv9_mod_env_t *env, const char *a, const char *b,
                    const char *c)
{
    m_say(env, RV9_STDERR, "execd: ");
    m_say(env, RV9_STDERR, a);
    m_say(env, RV9_STDERR, b);
    m_say(env, RV9_STDERR, c);
    m_say(env, RV9_STDERR, "\n");
}

/* The first frame: which command, and its argument. */
static int take_ask(const rv9_mod_env_t *env, void *ctx, char kind,
                    const uint8_t *data, uint32_t len)
{
    execd_statics_t *st = (execd_statics_t *)ctx;
    (void)env;
    if (kind != RUN_ASK || len == 0 || len >= ASK_MAX) return -1;
    for (uint32_t i = 0; i < len; i++) st->ask[i] = (char)data[i];
    st->ask[len] = '\0';
    return 1;
}

/* What the caller sends while the command runs: its input, then its end. */
static int take_input(const rv9_mod_env_t *env, void *ctx, char kind,
                      const uint8_t *data, uint32_t len)
{
    execd_statics_t *st = (execd_statics_t *)ctx;
    if (st->in_w < 0) return 0;
    if (kind == RUN_IN && len > 0) {
        env->write(st->in_w, data, len);
    } else if (kind == RUN_EOF) {
        env->close(st->in_w);
        st->in_w = -1;
    }
    return 0;
}

/* Send what is in a pipe now. 1 if something was, 0 if not, -1 at its end,
   -2 when it could not be sent: the caller is gone. */
static int carry(const rv9_mod_env_t *env, execd_statics_t *st, int from, char kind)
{
    uint32_t ready = 0;
    if (env->getstat(from, RV9_GS_READY, &ready) < 0 || ready == 0) return 0;
    int n = env->read(from, st->buf, sizeof(st->buf));
    if (n <= 0) return -1;
    if (run_send(env, RV9_STDOUT, kind, st->buf, (uint32_t)n) < 0) return -2;
    return 1;
}

/*
 * One caller: stdin and stdout are its connection, which arrives with the
 * caller's name on it.
 */
static int session(const rv9_mod_env_t *env, execd_statics_t *st, uint32_t seq)
{
    env->getstat(RV9_STDIN, RUN_GS_CALLER, st->who);
    st->who[sizeof(st->who) - 1] = '\0';

    /* Which command. Read as it comes; the connection waits for it. */
    st->have = 0;
    int got = 0;
    while (got == 0) {
        int n = env->read(RV9_STDIN, st->from + st->have, sizeof(st->from) - st->have);
        if (n <= 0) return 1;
        st->have += (uint32_t)n;
        got = run_frames(env, st, st->from, &st->have, take_ask);
        if (got < 0) return 1;
    }
    char *arg = st->ask;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = '\0';
    log_say(env, st->ask, " for ", st->who);

    /*
     * Its standard paths are three pipes, opened after ours are parked --
     * open() hands out the lowest free slot, and the shell learned what a
     * park landing on a fresh pipe costs.
     */
    env->dup2(RV9_STDIN, SAVE_IN);
    env->dup2(RV9_STDERR, SAVE_ERR);
    m_devpath(st->pipe[0], "/pipe/xi", seq);
    m_devpath(st->pipe[1], "/pipe/xo", seq);
    m_devpath(st->pipe[2], "/pipe/xe", seq);
    int in_w  = env->open(st->pipe[0], RV9_MODE_WRITE | RV9_MODE_CREATE);
    int in_r  = env->open(st->pipe[0], RV9_MODE_READ);
    int out_w = env->open(st->pipe[1], RV9_MODE_WRITE | RV9_MODE_CREATE);
    int out_r = env->open(st->pipe[1], RV9_MODE_READ);
    int err_w = env->open(st->pipe[2], RV9_MODE_WRITE | RV9_MODE_CREATE);
    int err_r = env->open(st->pipe[2], RV9_MODE_READ);

    int pid = -RV9_PE_NOMEM;
    if (in_w >= 0 && in_r >= 0 && out_w >= 0 && out_r >= 0 && err_w >= 0 && err_r >= 0) {
        env->dup2(in_r, RV9_STDIN);
        env->dup2(out_w, RV9_STDOUT);
        env->dup2(err_w, RV9_STDERR);
        pid = env->fork_arg(st->ask, 8, *arg ? arg : 0);
    } else {
        m_say(env, SAVE_ERR, "execd: no pipes left for a session (/pipe)\n");
    }
    env->dup2(SAVE_IN, RV9_STDIN);
    env->dup2(SAVE_IN, RV9_STDOUT);
    env->dup2(SAVE_ERR, RV9_STDERR);
    env->close(SAVE_IN);
    env->close(SAVE_ERR);
    /* The command has its ends now; ours are only the ones we read and
       write, so that its going away is an end of file here. */
    if (in_r >= 0)  env->close(in_r);
    if (out_w >= 0) env->close(out_w);
    if (err_w >= 0) env->close(err_w);

    uint8_t word[8];
    if (pid < 0) {
        run_put32(word, pid);
        run_send(env, RV9_STDOUT, RUN_REFUSED, word, 4);
        if (in_w >= 0)  env->close(in_w);
        if (out_r >= 0) env->close(out_r);
        if (err_r >= 0) env->close(err_r);
        log_say(env, st->ask, " was refused for ", st->who);
        return 1;
    }

    st->in_w = in_w;
    uint32_t one = 1;
    env->setstat(RV9_STDIN, RUN_SS_NOWAIT, &one);
    bool out_open = true, err_open = true, gone = false;

    /* What came with the request: a caller with its input ready sends it,
       and its end, straight after, and they arrive in the same read. Left
       for the next read, they would wait for bytes that are not coming. */
    if (st->have > 0 && run_frames(env, st, st->from, &st->have, take_input) < 0) {
        gone = true;
    }
    int status = 0, fault = RV9_FAULT_NONE;
    bool ended = false;

    while (!ended && !gone) {
        bool busy = false;

        int n = env->read(RV9_STDIN, st->from + st->have, sizeof(st->from) - st->have);
        if (n > 0) {
            st->have += (uint32_t)n;
            if (run_frames(env, st, st->from, &st->have, take_input) < 0) gone = true;
            busy = true;
        } else if (n != -RV9_IOE_WOULDBLOCK) {
            gone = true;                    /* hung up, or killed at that end */
        }
        if (gone) break;

        int c;
        if (out_open && (c = carry(env, st, out_r, RUN_OUT)) != 0) {
            busy = true;
            if (c == -2) gone = true;
            if (c < 0) out_open = false;
        }
        if (err_open && (c = carry(env, st, err_r, RUN_ERR)) != 0) {
            busy = true;
            if (c == -2) gone = true;
            if (c < 0) err_open = false;
        }
        if (gone) break;

        /* Ended, and everything it wrote sent: what it said last arrives
           before its status. Something it started may hold its pipes for
           longer, which is no reason to keep the caller waiting. */
        if (!busy) {
            if (env->wait_why(pid, &status, &fault, 0) == 0) ended = true;
            else env->sleep_ms(5);
        }
    }

    if (gone) {
        env->kill(pid);
        env->wait_why(pid, &status, &fault, 2000);
        log_say(env, st->ask, ": the caller hung up or went silent, so it was stopped "
                              "(its failsafes applied), for ", st->who);
    } else {
        run_put32(word, status);
        run_put32(word + 4, fault);
        run_send(env, RV9_STDOUT, RUN_EXIT, word, 8);
    }
    if (st->in_w >= 0) env->close(st->in_w);
    env->close(out_r);
    env->close(err_r);
    return 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 18) return 2;   /* /peer/listen */

    execd_statics_t *st = (execd_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    /* "session 7": one caller's, forked below with its connection. */
    const char *a = env->arg;
    if (a && a[0] == 's') {
        while (*a && *a != ' ') a++;
        while (*a == ' ') a++;
        return session(env, st, m_num_parse(a, 0));
    }

    for (;;) {
        /* Waits for a peer that proves it may ask. One that cannot is
           turned away inside the open, which then fails: listen again. */
        int c = env->open(LISTEN_PATH, RV9_MODE_RW);
        if (c < 0) {
            if (c != -RV9_IOE_DENIED && c != -RV9_IOE_IO) env->sleep_ms(1000);
            continue;
        }

        st->seq++;
        char arg[20];
        m_devpath(arg, "session ", st->seq);

        env->dup2(RV9_STDIN, SAVE_IN);
        env->dup2(RV9_STDOUT, SAVE_ERR);
        env->dup2(c, RV9_STDIN);
        env->dup2(c, RV9_STDOUT);
        int pid = env->fork_arg("execd", 8, arg);
        env->dup2(SAVE_IN, RV9_STDIN);
        env->dup2(SAVE_ERR, RV9_STDOUT);
        env->close(SAVE_IN);
        env->close(SAVE_ERR);
        env->close(c);
        if (pid < 0) m_say(env, RV9_STDERR, "execd: no session to be had\n");
    }
    return 0;
}
