/*
 * shell -- RV-9's command interpreter.
 *
 * An ordinary loadable module with no privileges. It reads lines from
 * stdin, forks other modules by name, and waits for them. Everything it
 * needs arrives through env: no libc, no globals, no kernel calls.
 *
 * There is almost no built-in command table, because a command is just a
 * module in the store. Adding a command means adding a module.
 *
 * Line editing, echo and blocking all happen below it in SCF, which is why
 * this file contains no terminal handling whatsoever.
 */
#include "modlib.h"

#define LINE_MAX  256
#define ARG_MAX   8
#define SAVE_PATH 5     /* spare slot used to park stdout during redirection */

/* Stages in one pipeline. Four is three pipes, which is more than anything
   worth typing, and the path table is what pays for each one. */
#define PIPE_STAGES 4

/* Where the shell parks its own stdout and stdin while a pipeline runs.
   Claimed before any pipe is opened, so that open() -- which hands out the
   lowest free slot -- can never be given one of them. */
#define SAVE_OUT  (SAVE_PATH)
#define SAVE_IN   (SAVE_PATH + 1)

/*
 * Commands joined by && or ||, on one line.
 *
 * Six is more than a legible line holds, and each one costs only a pointer
 * and a byte here -- the work is done one segment at a time, in the same
 * two buffers a single command uses.
 */
#define COND_MAX  6

#define COND_ALWAYS 0
#define COND_AND    1       /* run this only if the one before succeeded */
#define COND_OR     2       /* run this only if the one before did not */

typedef struct {
    char line[LINE_MAX];    /* chopped into tokens */
    char raw[LINE_MAX];     /* kept whole, for multi-word arguments */
    int  term;
    char term_open;
    uint32_t pipe_seq;      /* so two pipelines never share a pipe's name */

    /*
     * What the last command did, kept apart because they are two
     * different questions -- see the note above run(), and design §49.
     * `status` prints them; && and || only care whether it worked.
     */
    int  last_status;
    int  last_fault;

    /* Commands come from here when the shell was given a file to run, and
       from RV9_STDIN otherwise. */
    int  script;

    /* What has arrived and not yet been used. See read_line. */
    char in[LINE_MAX];
    int  in_len;

    rv9_sys_proc_t procs[16];   /* for the fallback `procs`, which cannot fork */
} shell_statics_t;

/*
 * One line, from wherever the commands are coming from.
 *
 * Three sources with three habits. A terminal hands back a line at a time,
 * because SCF does the editing. A socket hands back whatever the network
 * chose, so a line may take several reads or share one with the next. A
 * *file* hands back as much as was asked for, which is several lines at
 * once.
 *
 * The old reader looked for the first newline and threw away everything
 * after it. A terminal never noticed, a socket noticed rarely, and a script
 * would have run every other line -- silently, which is the worst way for a
 * script to be wrong. So what arrives is kept and consumed a line at a time.
 *
 * Returns the line's length, or -1 when the source is finished and nothing
 * is left over.
 */
static int read_line(const rv9_mod_env_t *env, shell_statics_t *st, int src)
{
    for (;;) {
        for (int i = 0; i < st->in_len; i++) {
            if (st->in[i] != '\n' && st->in[i] != '\r') continue;

            int len = i;
            for (int k = 0; k < len; k++) st->line[k] = st->in[k];
            st->line[len] = '\0';

            int drop = i + 1;
            if (drop < st->in_len && st->in[i] == '\r' && st->in[drop] == '\n') {
                drop++;                                  /* CRLF is one ending */
            }
            st->in_len -= drop;
            for (int k = 0; k < st->in_len; k++) st->in[k] = st->in[drop + k];
            return len;
        }

        /* Full, with no ending in it. Take what there is rather than wait
           for a newline that cannot fit. */
        if (st->in_len >= LINE_MAX - 1) {
            for (int k = 0; k < st->in_len; k++) st->line[k] = st->in[k];
            st->line[st->in_len] = '\0';
            int len = st->in_len;
            st->in_len = 0;
            return len;
        }

        int k = env->read(src, st->in + st->in_len, LINE_MAX - 1 - st->in_len);
        if (k <= 0) {
            if (st->in_len == 0) return -1;
            /* A last line with no newline after it is still a line, which
               is how most editors leave a file. */
            for (int i = 0; i < st->in_len; i++) st->line[i] = st->in[i];
            st->line[st->in_len] = '\0';
            int len = st->in_len;
            st->in_len = 0;
            return len;
        }
        st->in_len += k;
    }
}

/*
 * The way out, when there is no memory to start it.
 *
 * `kill` and `procs` are modules like every other command. But the moment
 * they matter most is the moment something has used up the memory or the
 * budget a fork needs -- and then the command that would stop it cannot be
 * started. So when forking one of them is refused for memory, the shell
 * does the same job itself, with nothing to allocate: the process table
 * goes into statics the shell already has, and a kill is two calls.
 */
static void fallback_procs(const rv9_mod_env_t *env)
{
    shell_statics_t *st = (shell_statics_t *)env->statics;
    int n = env->sysinfo(RV9_SYS_PROCS, st->procs, sizeof(st->procs));
    if (n > 16) n = 16;

    m_say(env, RV9_STDOUT, "(no memory to start procs; the shell's own)\n"
                           "pid   par   name        state\n");
    for (int i = 0; i < n; i++) {
        const rv9_sys_proc_t *p = &st->procs[i];
        m_numpad(env, RV9_STDOUT, p->pid, 6);
        m_numpad(env, RV9_STDOUT, p->parent, 6);
        m_pad(env, RV9_STDOUT, p->name, 12);
        m_say(env, RV9_STDOUT, p->state == 3 ? "exited\n" : "active\n");
    }
}

static void fallback_kill(const rv9_mod_env_t *env, const char *arg)
{
    const char *a = arg ? arg : "";
    while (*a == ' ') a++;
    bool force = (a[0] == '-' && a[1] == 'f');
    if (force) { a += 2; while (*a == ' ') a++; }

    const char *end = a;
    int pid = (int)m_num_parse(a, &end);
    if (pid <= 0 || end == a) {
        m_say(env, RV9_STDOUT, "usage: kill [-f] <pid>\n");
        return;
    }

    m_say(env, RV9_STDOUT, "(no memory to start kill; the shell's own)\n");
    int status = 0;
    if (!force && env->signal(pid, RV9_SIG_STOP) == 0 &&
        env->wait(pid, &status, 2000) == 0) {
        m_say(env, RV9_STDOUT, "stopped when asked\n");
        return;
    }
    m_say(env, RV9_STDOUT, env->kill(pid) == 0 ? "killed\n"
                                               : "could not be stopped\n");
}

/*
 * The argument is the rest of the line, not just the next token.
 *
 * tokenize() chops the line into words, so passing argv[1] hands a module
 * only the first of them -- which is how `wifi <ssid> <password>` arrived
 * at the driver with an empty password and sent us hunting through WPA
 * settings for a fault that was never there.
 */
static char *rest_of_line(char *raw)
{
    char *p = raw;
    while (*p && *p != ' ' && *p != '\t') p++;      /* past the command */
    while (*p == ' ' || *p == '\t') p++;            /* to the first argument */
    if (*p == '\0') return NULL;

    /* Stop before any redirection, in either direction: where the output
       goes and where the input comes from are the shell's business, not
       the module's, and a module handed "> /r0/x" as an argument would
       treat it as one. */
    for (char *r = p; *r; r++) {
        if (*r == '>' || *r == '<') {
            while (r > p && (r[-1] == ' ' || r[-1] == '\t')) r--;
            *r = '\0';
            break;
        }
    }

    return (*p == '\0') ? NULL : p;
}

/*
 * Chop a trailing "&" off the line and say whether it was there.
 *
 * It has been typed at this shell for months and silently handed to the
 * module as an argument, which is how `gauge &` came to hold the terminal
 * until it finished. Two things that run at once cannot be looked at from
 * a shell that only runs one.
 */
static bool take_ampersand(char *raw)
{
    char *end = raw;
    while (*end) end++;

    while (end > raw && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end == raw || end[-1] != '&') return false;

    end--;                                          /* drop the & */
    while (end > raw && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return true;
}

/*
 * Split a line in place on && and ||.
 *
 * This is the whole of "a script can make decisions". Without it a script
 * is a list of things that happen whatever went wrong before them, which
 * is a transcript rather than a program -- and RV-9's own demonstration is
 * meant to be run, not typed.
 *
 * `op[i]` says what joined this segment to the one before it, so the caller
 * evaluates left to right against a running status, exactly as `a && b ||
 * c` reads. The operators do not have precedence over each other and are
 * not meant to: anyone who needs precedence needs parentheses, and anyone
 * who needs parentheses needs a real language, which is upstairs in R9.
 *
 * `||` is looked for first, because `|` is a prefix of it and a pipeline
 * that swallowed the first bar of an or would be a bewildering way to
 * spend an evening.
 */
static int split_conditions(char *raw, char *seg[], uint8_t op[], int max)
{
    int  n  = 0;
    char *p = raw;

    seg[n]  = p;
    op[n++] = COND_ALWAYS;

    for (; *p; p++) {
        if (p[0] != '&' && p[0] != '|') continue;
        if (p[1] != p[0])               continue;       /* & or |, not && or || */
        if (n >= max)                   break;

        uint8_t kind = (p[0] == '&') ? COND_AND : COND_OR;

        /* Trim the segment that ends here, then start the next one after
           the operator. */
        char *e = p;
        while (e > seg[n - 1] && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = '\0';

        p += 2;
        while (*p == ' ' || *p == '\t') p++;

        seg[n]  = p;
        op[n++] = kind;
        p--;                                     /* the for() will step on */
    }

    return n;
}

/*
 * Open a redirection target, truncating or appending.
 *
 * RV9_MODE_CREATE truncates what it finds, which is right for `>` and
 * exactly wrong for `>>`. So append opens the file as it stands and only
 * creates when there is nothing there, then seeks to the end.
 *
 * `>>` exists because without it a file on this machine can only ever hold
 * what one command wrote. That was tolerable while files were places to put
 * output; it stopped being tolerable the moment the shell could *run* a
 * file, because a script with one line in it is not a script and there is
 * no editor on the board worth writing one in.
 */
static int open_target(const rv9_mod_env_t *env, const char *target,
                       bool append)
{
    if (!append) return env->open(target, RV9_MODE_WRITE | RV9_MODE_CREATE);

    int t = env->open(target, RV9_MODE_WRITE);
    if (t < 0) t = env->open(target, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (t >= 0) env->seek(t, 0, RV9_SEEK_END);
    return t;
}

/*
 * Pull `< file`, `> file` and `>> file` off a command, in place.
 *
 * This used to be done by walking the token list, which coupled it to
 * ARG_MAX -- eight. A redirection that fell past the eighth word was simply
 * not seen, and the command ran with its output still on the terminal:
 *
 *     echo # what the board can say for itself > /r0/demo
 *
 * wrote the comment to the console and created no file, silently, because
 * `>` was the tenth token. Nothing about where output goes has anything to
 * do with how many words precede it, so it is read off the line instead.
 *
 * The command's argument ends at the first redirection, which is also what
 * rest_of_line() assumes -- it finds nothing left to trim once this has
 * run.
 */
static void take_redirections(char *raw, const char **target, bool *append,
                              const char **source)
{
    *target = NULL;
    *source = NULL;
    *append = false;

    char *cut = NULL;

    for (char *p = raw; *p; ) {
        if (*p != '<' && *p != '>') { p++; continue; }

        bool out = (*p == '>');
        bool app = (out && p[1] == '>');
        if (cut == NULL) cut = p;

        char *n = p + (app ? 2 : 1);
        *p = '\0';                     /* ends the argument, or the name before */
        while (*n == ' ' || *n == '\t') n++;

        char *e = n;
        while (*e && *e != ' ' && *e != '\t' && *e != '<' && *e != '>') e++;

        char *next = e;
        if (*e != '\0' && *e != '<' && *e != '>') { *e = '\0'; next = e + 1; }

        if (*n != '\0') {
            if (out) { *target = n; *append = app; }
            else     { *source = n; }
        }
        p = next;
    }

    /* The spaces the redirection left on the end of the argument. */
    while (cut != NULL && cut > raw && (cut[-1] == ' ' || cut[-1] == '\t')) {
        *--cut = '\0';
    }
}

/* Split in place on whitespace. Returns the argument count. */
static int tokenize(char *line, char *argv[], int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return argc;
}

static void help(const rv9_mod_env_t *env)
{
    m_say(env, RV9_STDOUT,
          "commands are modules; 'mdir' lists them\n"
          "  help              this text\n"
          "  exit [n]          leave the shell, with a status\n"
          "  status            what the last command returned, or its fault\n"
          "  <module> [arg] [< file] [> dev] [>> file]  fork it, redirected\n"
          "  <module> | <module>     what one writes, the next reads\n"
          "  <module> &        run it without waiting; see it in 'procs'\n"
          "  a && b            b only if a succeeded;  a || b, only if not\n"
          "  kill [-f] <pid>   stop one: ask, then insist\n"
          "\n"
          "  shell <file>      run a file of commands; '#' is a comment\n"
          "\n"
          "try: mdir, procs, owns, pubs, free, dir, filetest, netstat\n"
          "     dir /r0        echo hello > /term\n"
          "     dir /f0 | count       mdir | match desc\n"
          "     fetch host /path > /r0/file\n"
          "     load /r0/file.mod     then run it by name\n");
}

/*
 * Why a fork was refused, in words.
 *
 * Shared by the plain path and by a pipeline, because a stage that will
 * not start is exactly as worth explaining as a command that will not --
 * and "did not start" sent somebody hunting for a broken pipe when the
 * answer was that the third process would not fit in memory.
 */
static void say_fork_error(const rv9_mod_env_t *env, const char *name, int pid)
{
    m_say(env, RV9_STDOUT, name);

        if (pid == -RV9_PE_NOMEM) {
            m_say(env, RV9_STDOUT, ": no memory to start it\n");
        } else if (pid == -RV9_PE_MODULE) {
            m_say(env, RV9_STDOUT, ": not loadable\n");
        } else if (pid == -RV9_PE_BUSY) {
            /* Refused before it started, because something it declared it
               must own alone is owned. 'owns' says by whom. */
            m_say(env, RV9_STDOUT, ": a device it needs alone is owned "
                                   "(see 'owns')\n");
        } else if (pid == -RV9_PE_NODEV) {
            m_say(env, RV9_STDOUT, ": it needs a device this machine does "
                                   "not have (see the log)\n");
        } else if (pid == -RV9_PE_NOPUB) {
            m_say(env, RV9_STDOUT, ": it watches a publication nothing on "
                                   "this machine provides (see the log)\n");
        } else if (pid == -RV9_PE_BUDGET) {
            m_say(env, RV9_STDOUT, ": over the memory budget of whatever is "
                                   "starting it (see 'budgets')\n");
        } else if (pid == -RV9_PE_UNSCHEDULABLE) {
            m_say(env, RV9_STDOUT, ": with it running, some real-time loop "
                                   "would miss its deadline (see the log)\n");
        } else if (pid == -RV9_PE_CONTRACT) {
            /* Reachable by an ordinary fork now, not only by 'rt': a
               failsafe naming a device the program never claimed is a
               manifest contradicting itself. */
            m_say(env, RV9_STDOUT, ": its declaration contradicts itself "
                                   "(see the log)\n");
        } else {
            m_say(env, RV9_STDOUT, ": no such module\n");
        }
}

/*
 * Split a command at the first '|' that is not at the very start.
 *
 * In place, because the shell has two LINE_MAX buffers and no business
 * allocating a third: each segment becomes its own string where the bar
 * used to be.
 */
static int split_pipeline(char *raw, char *seg[], int max)
{
    int n = 0;
    char *p = raw;

    seg[n++] = p;
    while (*p && n < max) {
        if (*p == '|') {
            *p = '\0';
            seg[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* Trim, then cut the first word off as the command name. What is left,
   with its spaces, is the argument -- see rest_of_line for why the whole
   remainder matters rather than just the next token. */
static char *cut_name(char *s, char **out_name)
{
    while (*s == ' ' || *s == '\t') s++;
    *out_name = s;

    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = '\0'; s++; }

    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/*
 * A pipeline: what one stage writes, the next one reads.
 *
 * The whole of it is opens, dup2 and fork -- there is no pipe machinery
 * here, because a pipe is a device and PIPEFM already is the machinery.
 * The shell's only jobs are to name each pipe, to point one child's output
 * and the next child's input at it, and then **to let go of both ends
 * itself**. That last part is not tidiness: a reader learns that a stage
 * has finished when the last writer closes, and the shell holding a write
 * end open is a writer that never finishes.
 *
 * Both ends are opened before either child is forked. Open only the write
 * end and the first stage can run, write, exit and take the pipe with it
 * before the reader ever arrives.
 */
static int run_pipeline(const rv9_mod_env_t *env, char *raw, bool background)
{
    shell_statics_t *st = (shell_statics_t *)env->statics;

    char *seg[PIPE_STAGES];
    int stages = split_pipeline(raw, seg, PIPE_STAGES);

    int pids[PIPE_STAGES];
    for (int i = 0; i < stages; i++) pids[i] = -1;

    /*
     * Park our own input and output *first*, before a single pipe is
     * opened.
     *
     * Not tidiness -- correctness. open() hands out the lowest free slot,
     * so parking into a fixed slot after opening a pipe lands on top of
     * the pipe, and putting the parked copy back afterwards closes it. The
     * read end of the first pipe was being destroyed that way before the
     * second stage could inherit it, and the only symptom was a command
     * that quietly produced nothing.
     *
     * Parked before anything else, these two slots are taken, and no open
     * in the loop below can be given them.
     */
    int saved_out = env->dup2(RV9_STDOUT, SAVE_OUT);
    int saved_in  = env->dup2(RV9_STDIN,  SAVE_IN);
    if (saved_out < 0 || saved_in < 0) {
        if (saved_out >= 0) env->close(SAVE_OUT);
        if (saved_in  >= 0) env->close(SAVE_IN);
        m_say(env, RV9_STDOUT, "no room to hold my own input and output\n");
        return -1;
    }

    int prev_read = -1;
    bool broken = false;

    for (int i = 0; i < stages && !broken; i++) {
        char *name = NULL;
        char *arg  = cut_name(seg[i], &name);
        if (name[0] == '\0') {
            m_say(env, RV9_STDERR, "empty stage in the pipeline\n");
            broken = true;
            break;
        }

        /* The first stage may take its input from a file, as any command
           may; the later ones take it from the stage before. */
        const char *source = NULL;
        if (i == 0) {
            for (char *q = arg; *q; q++) {
                if (*q == '<') {
                    *q = '\0';
                    char *f = q + 1;
                    while (*f == ' ') f++;
                    /* And the space before the bar. "a < f | b" split at
                       the bar leaves "f " -- which opens as a name with a
                       space on the end, and does not exist. */
                    char *e = f;
                    while (*e) e++;
                    while (e > f && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
                    if (*f) source = f;
                    break;
                }
            }
        }

        /* The last stage may still redirect, as any command may. */
        const char *target = NULL;
        bool        append = false;
        if (i == stages - 1) {
            for (char *q = arg; *q; q++) {
                if (*q == '>') {
                    append = (q[1] == '>');
                    *q = '\0';
                    char *t = q + (append ? 2 : 1);
                    while (*t == ' ') t++;
                    char *e = t;
                    while (*e) e++;
                    while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
                    if (*t) target = t;
                    break;
                }
            }
        }

        int pw = -1, pr = -1;
        if (i < stages - 1) {
            char pipename[24];
            m_devpath(pipename, "/pipe/s", st->pipe_seq++);

            /* Both ends before either child. Open only the write end and
               the first stage can run, write, exit and take the pipe with
               it before the reader ever arrives. */
            pw = env->open(pipename, RV9_MODE_WRITE | RV9_MODE_CREATE);
            if (pw >= 0) pr = env->open(pipename, RV9_MODE_READ);

            if (pw < 0 || pr < 0) {
                if (pw >= 0) env->close(pw);
                m_say(env, RV9_STDERR, pipename);
                m_say(env, RV9_STDERR, ": no pipe to be had\n");
                broken = true;
                break;
            }
        }

        if (target != NULL) {
            int t = open_target(env, target, append);
            if (t < 0) {
                m_say(env, RV9_STDERR, target);
                m_say(env, RV9_STDERR, ": cannot open\n");
                broken = true;
            } else {
                env->dup2(t, RV9_STDOUT);
                env->close(t);
            }
        } else if (pw >= 0) {
            env->dup2(pw, RV9_STDOUT);
        }

        if (!broken && source != NULL) {
            int f = env->open(source, RV9_MODE_READ);
            if (f < 0) {
                /*
                 * Standard error, not standard output: by this point this
                 * stage's output has been pointed at the pipe, so saying
                 * it on stdout would post the complaint into the very pipe
                 * whose reader is about to be abandoned. It was, and the
                 * command produced nothing at all.
                 */
                m_say(env, RV9_STDERR, source);
                m_say(env, RV9_STDERR, ": cannot open\n");
                broken = true;
            } else {
                env->dup2(f, RV9_STDIN);
                env->close(f);
            }
        } else if (!broken && prev_read >= 0) {
            env->dup2(prev_read, RV9_STDIN);
        }

        if (!broken) pids[i] = env->fork_arg(name, 8, arg);

        /* Our own back, from the copies parked before the loop. */
        env->dup2(SAVE_OUT, RV9_STDOUT);
        env->dup2(SAVE_IN,  RV9_STDIN);

        /* Let go of the ends; the children hold what they need. A reader
           learns a stage has finished when the last writer closes, and a
           shell still holding a write end is a writer that never does. */
        if (prev_read >= 0) env->close(prev_read);
        if (pw >= 0)        env->close(pw);
        prev_read = pr;

        /* Only when the fork itself was the failure; anything earlier has
           already said what went wrong in its own words. */
        if (!broken && pids[i] < 0) {
            say_fork_error(env, name, pids[i]);
            broken = true;
        }
    }

    if (prev_read >= 0) env->close(prev_read);

    env->dup2(SAVE_OUT, RV9_STDOUT);
    env->close(SAVE_OUT);
    env->dup2(SAVE_IN, RV9_STDIN);
    env->close(SAVE_IN);

    /*
     * Wait for all of them, not just the last. A stage still running when
     * the prompt comes back would write into a terminal the shell is
     * reading from, and the two would fight over it.
     */
    int last_status = 0;
    int last_fault  = RV9_FAULT_NONE;

    if (!background) {
        for (int i = 0; i < stages; i++) {
            if (pids[i] < 0) continue;
            int status = 0, fault = RV9_FAULT_NONE;
            env->wait_why(pids[i], &status, &fault, RV9_WAIT_FOREVER);

            /*
             * A pipeline's answer is its last stage's.
             *
             * Which is a choice, and the same one Unix makes by default.
             * `mdir | match nothing` failing because the match found
             * nothing is the useful reading; `mdir` having failed while
             * `count` cheerfully reported zero is the one that would be
             * missed, and it is missed here too. What stops that being a
             * trap is that a stage which dies says so on stderr, which a
             * pipe does not carry away.
             */
            if (i == stages - 1) { last_status = status; last_fault = fault; }
        }
    }

    shell_statics_t *sts = (shell_statics_t *)env->statics;
    sts->last_status = last_status;
    sts->last_fault  = last_fault;

    if (broken) return -1;
    return (last_fault != RV9_FAULT_NONE || last_status != 0) ? -1 : 0;
}

/*
 * Run a module, optionally with stdout pointed somewhere else.
 *
 * Redirection works because a child inherits its parent's standard paths by
 * reference. The shell parks its own stdout, aims stdout at the target,
 * forks -- the child gets the target without knowing -- then puts it back.
 */
static int run(const rv9_mod_env_t *env, const char *name, const char *arg,
               const char *target, const char *source, bool append,
               bool background)
{
    /*
     * Park first, open second -- both of them, before either.
     *
     * open() hands out the lowest free slot, so opening the target and
     * then parking into a fixed slot can land the parked copy on top of
     * the thing just opened. That cost an afternoon in the pipeline; it
     * is the same mistake available here.
     */
    int saved_out = -1, saved_in = -1;

    if (target != NULL) {
        saved_out = env->dup2(RV9_STDOUT, SAVE_OUT);
        if (saved_out < 0) {
            m_say(env, RV9_STDOUT, "cannot save stdout\n");
            return -1;
        }
    }
    if (source != NULL) {
        saved_in = env->dup2(RV9_STDIN, SAVE_IN);
        if (saved_in < 0) {
            if (saved_out >= 0) env->close(SAVE_OUT);
            m_say(env, RV9_STDOUT, "cannot save stdin\n");
            return -1;
        }
    }

    if (target != NULL) {
        /* CREATE so that "> /r0/thing" makes a file rather than failing.
           Redirecting to a device ignores it; redirecting to a volume is
           how anything gets onto one. */
        int t = open_target(env, target, append);
        if (t < 0) {
            m_say(env, RV9_STDOUT, target);
            m_say(env, RV9_STDOUT, ": cannot open\n");
            if (saved_in  >= 0) env->close(SAVE_IN);
            env->close(SAVE_OUT);
            return -1;
        }
        env->dup2(t, RV9_STDOUT);
        env->close(t);
    }
    if (source != NULL) {
        int f = env->open(source, RV9_MODE_READ);
        if (f < 0) {
            m_say(env, RV9_STDOUT, source);
            m_say(env, RV9_STDOUT, ": cannot open\n");
            if (saved_out >= 0) { env->dup2(SAVE_OUT, RV9_STDOUT);
                                  env->close(SAVE_OUT); }
            env->close(SAVE_IN);
            return -1;
        }
        env->dup2(f, RV9_STDIN);
        env->close(f);
    }

    int pid = env->fork_arg(name, 8, arg);
    int status = 0;
    int fault  = RV9_FAULT_NONE;

    bool fallback = false;
    if (pid == -RV9_PE_NOMEM || pid == -RV9_PE_BUDGET) {
        if (m_eq(name, "kill"))  { fallback_kill(env, arg); fallback = true; }
        if (m_eq(name, "procs")) { fallback_procs(env);     fallback = true; }
    }

    if (pid >= 0 && !background) {
        /* Until it finishes. Giving up after thirty seconds did not stop
           the command -- it put a second reader on the same terminal, and
           an editor and a shell then fought over every keystroke. */
        if (env->wait_why(pid, &status, &fault, RV9_WAIT_FOREVER) < 0) {
            status = -1;
        }
    }

    if (saved_out >= 0) {
        env->dup2(SAVE_OUT, RV9_STDOUT);
        env->close(SAVE_OUT);
    }
    if (saved_in >= 0) {
        env->dup2(SAVE_IN, RV9_STDIN);
        env->close(SAVE_IN);
    }

    if (pid < 0 && fallback) {
        /* Done already, by the shell itself. */
    } else if (pid < 0) {
        say_fork_error(env, name, pid);
    } else if (background) {
        /* No job table and no notification when it ends: `procs` is where
           to look. What this buys is two things running at once, which is
           the whole of what was missing. */
        m_say(env, RV9_STDOUT, "[");
        m_num(env, RV9_STDOUT, pid);
        m_say(env, RV9_STDOUT, "] ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, "\n");
    } else if (fault != RV9_FAULT_NONE) {
        /*
         * Ended, not returned -- and the *fault* is what says so.
         *
         * This used to test the status: -12 meant killed, -13 meant a
         * missed deadline. The comment here said those statuses were
         * "RV-9's, never the program's", and that was simply untrue. A
         * module's return shares the number space, so `i2c` returning -6
         * for "that address did not answer" was announced as having been
         * stopped by the scheduler. It had run perfectly.
         *
         * env->wait_why hands back both, and only one of them is RV-9
         * speaking.
         */
        m_say(env, RV9_STDOUT, name);
        if (fault == RV9_FAULT_KILLED) {
            m_say(env, RV9_STDOUT, ": killed\n");
        } else if (fault == RV9_FAULT_DEADLINE) {
            m_say(env, RV9_STDOUT, ": missed its deadline and was stopped\n");
        } else if (fault == RV9_FAULT_RUNAWAY) {
            m_say(env, RV9_STDOUT, ": stopped waiting for its releases, and "
                                   "was stopped\n");
        } else if (fault == RV9_FAULT_STACK) {
            m_say(env, RV9_STDOUT, ": ran off its stack and was stopped\n");
        } else {
            m_say(env, RV9_STDOUT, ": stopped by the scheduler (see the log)\n");
        }
    } else if (status != 0) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, " returned ");
        m_num(env, RV9_STDOUT, status);
        m_say(env, RV9_STDOUT, "\n");
    }

    /*
     * Both kept, one returned.
     *
     * `status` reports the pair, because they answer different questions.
     * && and || want one bit, and the bit has to count a fault as failure:
     * a program that was stopped for running off its stack did not succeed,
     * whatever number happened to be in its status register.
     *
     * A backgrounded command has not finished, so there is nothing to
     * report about it -- starting it is the success.
     */
    shell_statics_t *st = (shell_statics_t *)env->statics;
    if (pid < 0) {
        st->last_status = pid;
        st->last_fault  = RV9_FAULT_NONE;
        return fallback ? 0 : -1;
    }
    if (background) {
        st->last_status = 0;
        st->last_fault  = RV9_FAULT_NONE;
        return 0;
    }

    st->last_status = status;
    st->last_fault  = fault;
    return (fault != RV9_FAULT_NONE || status != 0) ? -1 : 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)                            return -1;
    if (env->abi_version < 7)                   return -2;
    if (env->read == NULL || env->fork == NULL) return -3;

    shell_statics_t *st = (shell_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -4;

    st->in_len      = 0;
    st->last_status = 0;
    st->last_fault  = RV9_FAULT_NONE;
    st->script      = -1;

    /*
     * Given a file, run it instead of talking to anybody.
     *
     *     shell /f0/demo
     *
     * The same parser either way, which is the entire reason this lives in
     * the shell rather than in a `script` command: a script wants pipes and
     * redirection, and those are here. A script is not a second language.
     *
     * No banner and no prompt when scripted -- a prompt printed into a log
     * is noise, and the point of a script is that nobody is watching.
     */
    if (env->arg != NULL) {
        const char *path = env->arg;
        while (*path == ' ' || *path == '\t') path++;
        if (*path != '\0') {
            st->script = env->open(path, RV9_MODE_READ);
            if (st->script < 0) {
                m_say(env, RV9_STDERR, "shell: cannot open ");
                m_say(env, RV9_STDERR, path);
                m_say(env, RV9_STDERR, "\n");
                return 1;
            }
        }
    }

    bool scripted = (st->script >= 0);
    int  src      = scripted ? st->script : RV9_STDIN;

    /* Mirror activity to the panel, so the board shows what is happening
       even though the keyboard is at the other end of the cable. */
    st->term = env->open("/term", RV9_MODE_WRITE);
    st->term_open = (st->term >= 0);

    if (!scripted) {
        m_say(env, RV9_STDOUT, "\nRV-9 shell. Type 'help'.\n");
        if (st->term_open) m_say(env, st->term, "shell ready\n");
    }

    int leaving = -1;            /* the status `exit` asked for, once it has */

    while (leaving < 0) {
        if (!scripted) m_say(env, RV9_STDOUT, "rv9> ");

        int n = read_line(env, st, src);
        if (n < 0) break;                       /* the source is finished */

        /*
         * Blank lines and comments.
         *
         * A script with no way to say why it does something is a script
         * nobody will trust enough to run, so `#` is not a luxury.
         */
        const char *first = st->line;
        while (*first == ' ' || *first == '\t') first++;
        if (*first == '\0' || *first == '#') continue;

        /* Keep a whole copy: tokenize() chops, and every segment needs its
           own argument with the spaces still in it. */
        for (int i = 0; i <= n; i++) st->raw[i] = st->line[i];

        char   *seg[COND_MAX];
        uint8_t op[COND_MAX];
        int     nseg = split_conditions(st->raw, seg, op, COND_MAX);

        /* The running answer && and || are asked about, left to right. */
        int outcome = 0;

        for (int s = 0; s < nseg && leaving < 0; s++) {
            if (s > 0) {
                if (op[s] == COND_AND && outcome != 0) continue;
                if (op[s] == COND_OR  && outcome == 0) continue;
            }

            char *raw = seg[s];
            bool background = take_ampersand(raw);

            /* A pipeline parses its own redirections, one for the first
               stage and one for the last, so it has to be spotted before
               anything is pulled off this line. */
            bool piped = false;
            for (const char *q = raw; *q; q++) if (*q == '|') piped = true;

            const char *target = NULL, *source = NULL;
            bool append = false;
            if (!piped) take_redirections(raw, &target, &append, &source);

            int k = 0;
            while (raw[k] != '\0' && k < LINE_MAX - 1) { st->line[k] = raw[k]; k++; }
            st->line[k] = '\0';

            char *argv[ARG_MAX];
            int argc = tokenize(st->line, argv, ARG_MAX);
            if (argc == 0) continue;

            if (m_eq(argv[0], "exit")) {
                /*
                 * A script says how it went by how it leaves, so `exit`
                 * takes a number. Whoever ran the script -- another script,
                 * or a shell with && after it -- reads that and nothing
                 * else.
                 */
                const char *a = rest_of_line(raw);
                int code = 0;
                if (a != NULL) {
                    while (*a == ' ') a++;
                    bool neg = (*a == '-');
                    if (neg) a++;
                    const char *end = a;
                    code = (int)m_num_parse(a, &end);
                    if (neg) code = -code;
                }
                leaving = code;
                break;
            }

            if (m_eq(argv[0], "help")) { help(env); outcome = 0; continue; }

            if (m_eq(argv[0], "status")) {
                /*
                 * Deliberately transparent: it reports and does not become
                 * the thing reported, so `cmd || status` says what went
                 * wrong rather than what `status` did.
                 */
                if (st->last_fault != RV9_FAULT_NONE) {
                    m_say(env, RV9_STDOUT, "stopped by RV-9, fault ");
                    m_num(env, RV9_STDOUT, st->last_fault);
                    m_say(env, RV9_STDOUT, "\n");
                } else {
                    m_say(env, RV9_STDOUT, "returned ");
                    m_num(env, RV9_STDOUT, st->last_status);
                    m_say(env, RV9_STDOUT, "\n");
                }
                continue;
            }

            if (piped) {
                if (st->term_open && m_onscreen(env, st->term)) {
                    m_say(env, st->term, "> ");
                    m_say(env, st->term, argv[0]);
                    m_say(env, st->term, " |\n");
                }
                outcome = run_pipeline(env, raw, background);
                continue;
            }

            const char *arg = rest_of_line(raw);

            /*
             * Mirror to the panel, but never take it back.
             *
             * Showing what is being typed is a convenience for somebody
             * glancing at the board. Once a program has drawn on the panel --
             * a chart, a picture -- that convenience would wipe it at the next
             * command, which is a poor trade for a line of text nobody asked
             * to see. Writing to /term deliberately still brings the console
             * back.
             */
            if (st->term_open && m_onscreen(env, st->term)) {
                m_say(env, st->term, "> ");
                m_say(env, st->term, argv[0]);
                m_say(env, st->term, "\n");
            }

            outcome = run(env, argv[0], arg, target, source, append, background);
        }
    }

    if (st->term_open) env->close(st->term);
    if (st->script >= 0) env->close(st->script);

    if (!scripted) m_say(env, RV9_STDOUT, "shell exiting\n");
    return (leaving > 0) ? leaving : 0;
}
