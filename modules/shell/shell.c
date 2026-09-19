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

typedef struct {
    char line[LINE_MAX];    /* chopped into tokens */
    char raw[LINE_MAX];     /* kept whole, for multi-word arguments */
    int  term;
    char term_open;
    uint32_t pipe_seq;      /* so two pipelines never share a pipe's name */
    rv9_sys_proc_t procs[16];   /* for the fallback `procs`, which cannot fork */
} shell_statics_t;

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
          "  exit              leave the shell\n"
          "  <module> [arg] [< file] [> dev]  fork it, redirected\n"
          "  <module> | <module>     what one writes, the next reads\n"
          "  <module> &        run it without waiting; see it in 'procs'\n"
          "  kill [-f] <pid>   stop one: ask, then insist\n"
          "\n"
          "try: mdir, procs, owns, pubs, free, dir, filetest, netstat\n"
          "     dir /r0        echo > /term\n"
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
static void run_pipeline(const rv9_mod_env_t *env, char *raw, bool background)
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
        return;
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
        if (i == stages - 1) {
            for (char *q = arg; *q; q++) {
                if (*q == '>') {
                    *q = '\0';
                    char *t = q + 1;
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
            int t = env->open(target, RV9_MODE_WRITE | RV9_MODE_CREATE);
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
    if (!background) {
        for (int i = 0; i < stages; i++) {
            if (pids[i] < 0) continue;
            int status = 0;
            env->wait(pids[i], &status, RV9_WAIT_FOREVER);
        }
    }
}

/*
 * Run a module, optionally with stdout pointed somewhere else.
 *
 * Redirection works because a child inherits its parent's standard paths by
 * reference. The shell parks its own stdout, aims stdout at the target,
 * forks -- the child gets the target without knowing -- then puts it back.
 */
static void run(const rv9_mod_env_t *env, const char *name, const char *arg,
                const char *target, const char *source, bool background)
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
            return;
        }
    }
    if (source != NULL) {
        saved_in = env->dup2(RV9_STDIN, SAVE_IN);
        if (saved_in < 0) {
            if (saved_out >= 0) env->close(SAVE_OUT);
            m_say(env, RV9_STDOUT, "cannot save stdin\n");
            return;
        }
    }

    if (target != NULL) {
        /* CREATE so that "> /r0/thing" makes a file rather than failing.
           Redirecting to a device ignores it; redirecting to a volume is
           how anything gets onto one. */
        int t = env->open(target, RV9_MODE_WRITE | RV9_MODE_CREATE);
        if (t < 0) {
            m_say(env, RV9_STDOUT, target);
            m_say(env, RV9_STDOUT, ": cannot open\n");
            if (saved_in  >= 0) env->close(SAVE_IN);
            env->close(SAVE_OUT);
            return;
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
            return;
        }
        env->dup2(f, RV9_STDIN);
        env->close(f);
    }

    int pid = env->fork_arg(name, 8, arg);
    int status = 0;

    bool fallback = false;
    if (pid == -RV9_PE_NOMEM || pid == -RV9_PE_BUDGET) {
        if (m_eq(name, "kill"))  { fallback_kill(env, arg); fallback = true; }
        if (m_eq(name, "procs")) { fallback_procs(env);     fallback = true; }
    }

    if (pid >= 0 && !background) {
        /* Until it finishes. Giving up after thirty seconds did not stop
           the command -- it put a second reader on the same terminal, and
           an editor and a shell then fought over every keystroke. */
        if (env->wait(pid, &status, RV9_WAIT_FOREVER) < 0) status = -1;
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
    } else if (status == -RV9_PE_KILLED) {
        /* Ended, not returned: these statuses are RV-9's, never the
           program's, and "returned -12" would say otherwise. */
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": killed\n");
    } else if (status == -RV9_PE_DEADLINE) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": missed its deadline and was stopped\n");
    } else if (status == -RV9_PE_RUNAWAY) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": stopped waiting for its releases, and "
                               "was stopped\n");
    } else if (status == -RV9_PE_FAULT) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": stopped by the scheduler (see the log)\n");
    } else if (status != 0) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, " returned ");
        m_num(env, RV9_STDOUT, status);
        m_say(env, RV9_STDOUT, "\n");
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL)                            return -1;
    if (env->abi_version < 7)                   return -2;
    if (env->read == NULL || env->fork == NULL) return -3;

    shell_statics_t *st = (shell_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -4;

    /* Mirror activity to the panel, so the board shows what is happening
       even though the keyboard is at the other end of the cable. */
    st->term = env->open("/term", RV9_MODE_WRITE);
    st->term_open = (st->term >= 0);

    m_say(env, RV9_STDOUT, "\nRV-9 shell. Type 'help'.\n");
    if (st->term_open) m_say(env, st->term, "shell ready\n");

    for (;;) {
        m_say(env, RV9_STDOUT, "rv9> ");

        /*
         * Read until a line is complete.
         *
         * Over a terminal, SCF hands back a whole line at once. Over a
         * socket the bytes arrive in whatever sizes the network chose, and
         * a line may take several reads or share one with the next. The
         * newline is what ends a line, whichever it came from.
         */
        int n = 0;
        bool closed = false, complete = false;

        while (!complete && n < LINE_MAX - 1) {
            int k = env->read(RV9_STDIN, st->line + n, LINE_MAX - 1 - n);
            if (k <= 0) { closed = true; break; }

            for (int i = n; i < n + k; i++) {
                if (st->line[i] == '\n' || st->line[i] == '\r') {
                    complete = true;
                    break;
                }
            }
            n += k;
        }

        if (closed && n == 0) break;     /* the other end went away */

        st->line[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (st->line[i] == '\n' || st->line[i] == '\r') {
                st->line[i] = '\0';
                n = i;
                break;
            }
        }
        if (n == 0) continue;

        /* Keep a whole copy before tokenize() chops the original. */
        for (int i = 0; i <= n; i++) st->raw[i] = st->line[i];

        /* Off both copies, before either is parsed. */
        bool background = take_ampersand(st->raw);
        if (background) take_ampersand(st->line);

        char *argv[ARG_MAX];
        int argc = tokenize(st->line, argv, ARG_MAX);
        if (argc == 0) continue;

        if (m_eq(argv[0], "exit")) break;
        if (m_eq(argv[0], "help")) { help(env); continue; }

        /*
         * A pipeline is handled whole, from the untouched copy: tokenize()
         * has already chopped st->line, and every stage needs its own
         * argument with the spaces still in it.
         */
        bool piped = false;
        for (const char *q = st->raw; *q; q++) if (*q == '|') piped = true;

        if (piped) {
            if (st->term_open && m_onscreen(env, st->term)) {
                m_say(env, st->term, "> ");
                m_say(env, st->term, argv[0]);
                m_say(env, st->term, " |\n");
            }
            run_pipeline(env, st->raw, background);
            continue;
        }

        /* "cmd arg > /dev" -- pull the redirection off the end, and pass
           whatever is left as the module's argument. */
        const char *target = NULL, *source = NULL;
        for (int i = 1; i < argc; i++) {
            if (m_eq(argv[i], ">") && i + 1 < argc) target = argv[i + 1];
            if (m_eq(argv[i], "<") && i + 1 < argc) source = argv[i + 1];
        }
        const char *arg = rest_of_line(st->raw);

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

        run(env, argv[0], arg, target, source, background);
    }

    if (st->term_open) env->close(st->term);
    m_say(env, RV9_STDOUT, "shell exiting\n");
    return 0;
}
