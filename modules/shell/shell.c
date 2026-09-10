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

#define LINE_MAX  128
#define ARG_MAX   8
#define SAVE_PATH 5     /* spare slot used to park stdout during redirection */

typedef struct {
    char line[LINE_MAX];
    int  term;
    char term_open;
} shell_statics_t;

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
          "\n"
          "try: mdir, procs, free, dir, filetest\n"
          "     dir /r0        echo > /term\n"
          "     del /r0/notes.txt\n");
}

/*
 * Run a module, optionally with stdout pointed somewhere else.
 *
 * Redirection works because a child inherits its parent's standard paths by
 * reference. The shell parks its own stdout, aims stdout at the target,
 * forks -- the child gets the target without knowing -- then puts it back.
 */
static void run(const rv9_mod_env_t *env, const char *name, const char *arg,
                const char *target)
{
    int redirected = 0;

    if (target != NULL) {
        if (env->dup2(RV9_STDOUT, SAVE_PATH) < 0) {
            m_say(env, RV9_STDOUT, "cannot save stdout\n");
            return;
        }
        int t = env->open(target, RV9_MODE_WRITE);
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

    if (pid >= 0) {
        if (env->wait(pid, &status, 30000) < 0) status = -1;
    }

    if (redirected) {
        env->dup2(SAVE_PATH, RV9_STDOUT);
        env->close(SAVE_PATH);
    }

    if (pid < 0) {
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, ": no such module\n");
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

        int n = env->read(RV9_STDIN, st->line, LINE_MAX - 1);
        if (n < 0) {
            m_say(env, RV9_STDOUT, "read error\n");
            break;
        }
        st->line[n] = '\0';
        if (n == 0) continue;

        char *argv[ARG_MAX];
        int argc = tokenize(st->line, argv, ARG_MAX);
        if (argc == 0) continue;

        if (m_eq(argv[0], "exit")) break;
        if (m_eq(argv[0], "help")) { help(env); continue; }

        /* "cmd arg > /dev" -- pull the redirection off the end, and pass
           whatever is left as the module's argument. */
        const char *target = NULL;
        const char *arg = NULL;
        for (int i = 1; i < argc; i++) {
            if (m_eq(argv[i], ">") && i + 1 < argc) {
                target = argv[i + 1];
                break;
            }
            if (arg == NULL) arg = argv[i];
        }

        if (st->term_open) {
            m_say(env, st->term, "> ");
            m_say(env, st->term, argv[0]);
            m_say(env, st->term, "\n");
        }

        run(env, argv[0], arg, target);
    }

    if (st->term_open) env->close(st->term);
    m_say(env, RV9_STDOUT, "shell exiting\n");
    return 0;
}
