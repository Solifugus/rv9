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

typedef struct {
    char line[LINE_MAX];    /* chopped into tokens */
    char raw[LINE_MAX];     /* kept whole, for multi-word arguments */
    int  term;
    char term_open;
} shell_statics_t;

/*
 * The argument is the rest of the line, not just the next token.
 *
 * tokenize() chops the line into words, so passing argv[1] hands a module
 * only the first of them -- which is how `wifi <ssid> <password>` arrived
 * at the driver with an empty password and sent us hunting through WPA
 * settings for a fault that was never there.
 */
static char *rest_of_line(char *raw, const char *redirect_target)
{
    char *p = raw;
    while (*p && *p != ' ' && *p != '\t') p++;      /* past the command */
    while (*p == ' ' || *p == '\t') p++;            /* to the first argument */
    if (*p == '\0') return NULL;

    /* Stop before any redirection, which is the shell's business. */
    if (redirect_target != NULL) {
        for (char *r = p; *r; r++) {
            if (*r == '>') {
                while (r > p && (r[-1] == ' ' || r[-1] == '\t')) r--;
                *r = '\0';
                break;
            }
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
          "  <module> [arg] [> dev]  fork it, optionally redirected\n"
          "  <module> &        run it without waiting; see it in 'procs'\n"
          "\n"
          "try: mdir, procs, owns, free, dir, filetest, netstat\n"
          "     dir /r0        echo > /term\n"
          "     fetch host /path > /r0/file\n"
          "     load /r0/file.mod     then run it by name\n");
}

/*
 * Run a module, optionally with stdout pointed somewhere else.
 *
 * Redirection works because a child inherits its parent's standard paths by
 * reference. The shell parks its own stdout, aims stdout at the target,
 * forks -- the child gets the target without knowing -- then puts it back.
 */
static void run(const rv9_mod_env_t *env, const char *name, const char *arg,
                const char *target, bool background)
{
    int redirected = 0;

    if (target != NULL) {
        if (env->dup2(RV9_STDOUT, SAVE_PATH) < 0) {
            m_say(env, RV9_STDOUT, "cannot save stdout\n");
            return;
        }
        /* CREATE so that "> /r0/thing" makes a file rather than failing.
           Redirecting to a device ignores it; redirecting to a volume is
           how anything gets onto one. */
        int t = env->open(target, RV9_MODE_WRITE | RV9_MODE_CREATE);
        if (t < 0) {
            m_say(env, RV9_STDOUT, target);
            m_say(env, RV9_STDOUT, ": cannot open\n");
            env->close(SAVE_PATH);
            return;
        }
        env->dup2(t, RV9_STDOUT);
        env->close(t);
        redirected = 1;
    }

    int pid = env->fork_arg(name, 8, arg);
    int status = 0;

    if (pid >= 0 && !background) {
        /* Until it finishes. Giving up after thirty seconds did not stop
           the command -- it put a second reader on the same terminal, and
           an editor and a shell then fought over every keystroke. */
        if (env->wait(pid, &status, RV9_WAIT_FOREVER) < 0) status = -1;
    }

    if (redirected) {
        env->dup2(SAVE_PATH, RV9_STDOUT);
        env->close(SAVE_PATH);
    }

    if (pid < 0) {
        /* Which failure it was. "No such module" for an exhausted heap is
           a message that sends you hunting for the wrong thing. */
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
        } else {
            m_say(env, RV9_STDOUT, ": no such module\n");
        }
    } else if (background) {
        /* No job table and no notification when it ends: `procs` is where
           to look. What this buys is two things running at once, which is
           the whole of what was missing. */
        m_say(env, RV9_STDOUT, "[");
        m_num(env, RV9_STDOUT, pid);
        m_say(env, RV9_STDOUT, "] ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, "\n");
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

        /* "cmd arg > /dev" -- pull the redirection off the end, and pass
           whatever is left as the module's argument. */
        const char *target = NULL;
        for (int i = 1; i < argc; i++) {
            if (m_eq(argv[i], ">") && i + 1 < argc) {
                target = argv[i + 1];
                break;
            }
        }
        const char *arg = rest_of_line(st->raw, target);

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

        run(env, argv[0], arg, target, background);
    }

    if (st->term_open) env->close(st->term);
    m_say(env, RV9_STDOUT, "shell exiting\n");
    return 0;
}
