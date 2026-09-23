/*
 * calc -- arithmetic, as a program.   calc 3 + 4
 *
 *     calc A + B      calc A - B
 *     calc A x B      calc A / B      calc A % B
 *
 * Writes the answer to standard output and nothing else, so the shell can
 * keep it:
 *
 *     set N = calc $N + 1
 *
 * WHY THIS IS A MODULE AND NOT SHELL SYNTAX
 *
 * The same reason as `compare`. Arithmetic in a shell means an expression
 * parser in the shell, and an expression parser needs precedence, and
 * precedence needs parentheses, and by then the shell is a language. Out
 * here it is forty lines that can be replaced without touching the thing
 * that runs scripts.
 *
 * `x` for multiply, because `*` is not special to this shell today and
 * relying on that is how a glob added later breaks every script ever
 * written. A word that was never going to mean anything else is cheaper than
 * a rule about quoting.
 *
 * Integers only, 32-bit, and no expressions -- two operands and one
 * operator. A script that needs more than this needs R9, and saying so is
 * more useful than half an expression evaluator.
 */
#include "modlib.h"

static bool as_number(const char *s, int32_t *out)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return false;

    bool neg = (*s == '-');
    if (neg || *s == '+') s++;
    if (*s < '0' || *s > '9') return false;

    int32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (v > 200000000) return false;
        v = v * 10 + (*s - '0');
    }

    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') return false;

    *out = neg ? -v : v;
    return true;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return 2;
    if (env->arg == NULL) {
        m_say(env, RV9_STDERR, "usage: calc A <+|-|x|/|%> B\n");
        return 2;
    }

    char a[16], op[4], b[16];
    const char *p = m_word(env->arg, a, sizeof(a));
    p = m_word(p, op, sizeof(op));
    m_word(p, b, sizeof(b));

    int32_t x = 0, y = 0;
    if (!as_number(a, &x) || !as_number(b, &y) || op[0] == '\0') {
        m_say(env, RV9_STDERR, "calc: needs a number, an operator and a "
                               "number\n");
        return 2;
    }

    int32_t r;
    if      (m_eq(op, "+")) r = x + y;
    else if (m_eq(op, "-")) r = x - y;
    else if (m_eq(op, "x")) r = x * y;
    else if (m_eq(op, "/") || m_eq(op, "%")) {
        if (y == 0) {
            /*
             * Refused, not reported as zero. A script dividing by zero has a
             * bug, and an answer of nought would let it carry on carrying
             * the bug -- which is the failure this whole system's error
             * handling is arranged to prevent.
             */
            m_say(env, RV9_STDERR, "calc: divide by zero\n");
            return 3;
        }
        r = m_eq(op, "/") ? (x / y) : (x % y);
    } else {
        m_say(env, RV9_STDERR, "calc: '");
        m_say(env, RV9_STDERR, op);
        m_say(env, RV9_STDERR, "' is not one of + - x / %\n");
        return 2;
    }

    m_num(env, RV9_STDOUT, r);
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
