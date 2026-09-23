/*
 * compare -- is this true?   compare 5 lt 10
 *
 *     compare A eq B      equal
 *     compare A ne B      not equal
 *     compare A lt B      less than
 *     compare A le B      less than or equal
 *     compare A gt B      greater than
 *     compare A ge B      greater than or equal
 *
 * Returns 0 when true and 1 when false, so it reads correctly in the shell:
 *
 *     if compare $D lt $LIMIT
 *         pin 4 0
 *     end
 *
 * WHY THIS IS A MODULE AND NOT SHELL SYNTAX
 *
 * Because comparison is a program. The shell needs control flow -- which
 * lines run, and in what order -- and nothing else. Every piece of *logic*
 * that lives out here stays separately loadable, separately replaceable and
 * separately testable, and the shell stops growing. A shell that grows an
 * operator every time somebody needs to compare two things ends up being a
 * language, and there is already a language: R9.
 *
 * Words rather than symbols, and for once not only for readability. `<` and
 * `>` are redirection to this shell, so `compare $D < 300` would open a file
 * called 300. Spelling it `lt` removes the trap rather than documenting it.
 *
 * NUMBERS WHEN IT CAN, TEXT WHEN IT CANNOT
 *
 * Both sides parsed as integers if both look like integers -- so `compare 9
 * lt 10` is true, where a text comparison would say otherwise and be
 * useless for the thing this exists to do. Otherwise text, and then only
 * `eq` and `ne` mean anything: whether one word sorts before another is a
 * question `sort` answers about lists, not one to invent an answer to here.
 */
#include "modlib.h"

/* Signed, because a distance can be -1 for "no reading" and a script must be
   able to test for it. */
static bool as_number(const char *s, int32_t *out)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return false;

    bool neg = (*s == '-');
    if (neg || *s == '+') s++;
    if (*s < '0' || *s > '9') return false;

    int32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if (v > 1000000000) return false;          /* not a number we handle */
    }

    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') return false;                  /* trailing rubbish */

    *out = neg ? -v : v;
    return true;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return 2;
    if (env->arg == NULL) {
        m_say(env, RV9_STDERR,
              "usage: compare A <eq|ne|lt|le|gt|ge> B\n");
        return 2;
    }

    char a[24], op[4], b[24];
    const char *p = m_word(env->arg, a, sizeof(a));
    p = m_word(p, op, sizeof(op));
    m_word(p, b, sizeof(b));

    if (a[0] == '\0' || op[0] == '\0' || b[0] == '\0') {
        m_say(env, RV9_STDERR, "compare: needs three words\n");
        return 2;
    }

    int32_t x = 0, y = 0;
    bool numeric = as_number(a, &x) && as_number(b, &y);

    bool answer;
    if (m_eq(op, "eq")) {
        answer = numeric ? (x == y) : m_eq(a, b);
    } else if (m_eq(op, "ne")) {
        answer = numeric ? (x != y) : !m_eq(a, b);
    } else if (!numeric) {
        /*
         * Refusing rather than guessing. Ordering two things that are not
         * numbers has an answer, but not one this command has any business
         * inventing -- and a script that silently took a wrong branch
         * because "abc" was somehow less than "abd" would be worse served
         * than one that is told its comparison made no sense.
         */
        m_say(env, RV9_STDERR, "compare: ");
        m_say(env, RV9_STDERR, op);
        m_say(env, RV9_STDERR, " needs two numbers; got '");
        m_say(env, RV9_STDERR, a);
        m_say(env, RV9_STDERR, "' and '");
        m_say(env, RV9_STDERR, b);
        m_say(env, RV9_STDERR, "'\n");
        return 2;
    } else if (m_eq(op, "lt")) { answer = (x <  y);
    } else if (m_eq(op, "le")) { answer = (x <= y);
    } else if (m_eq(op, "gt")) { answer = (x >  y);
    } else if (m_eq(op, "ge")) { answer = (x >= y);
    } else {
        m_say(env, RV9_STDERR, "compare: '");
        m_say(env, RV9_STDERR, op);
        m_say(env, RV9_STDERR, "' is not one of eq ne lt le gt ge\n");
        return 2;
    }

    /* 0 is true, because 0 is what the shell reads as success. 2 is
       "the question was wrong", which is not the same as "no". */
    return answer ? 0 : 1;
}
