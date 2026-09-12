/*
 * Helpers every RV-9 module needs and cannot get from libc.
 *
 * Header-only and static: each module links its own copy, which keeps
 * modules self-contained blobs with no external symbols -- the rule that
 * makes them position-independent.
 *
 * TODO: RV9_MOD_LIBRARY exists as a module type but nothing links against
 * libraries yet. When it does, this becomes a real shared library and the
 * duplication goes away.
 */
#pragma once

#include "rv9/module.h"

static inline uint32_t m_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int m_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline void m_say(const rv9_mod_env_t *env, int path, const char *s)
{
    env->write(path, s, m_len(s));
}

/* Signed decimal. No printf here. */
static inline void m_num(const rv9_mod_env_t *env, int path, int32_t v)
{
    char buf[12];
    int i = sizeof(buf);
    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;

    buf[--i] = '\0';
    do { buf[--i] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (v < 0) buf[--i] = '-';

    m_say(env, path, &buf[i]);
}

/* Decimal, stopping at the first character that is not a digit. */
static inline uint32_t m_num_parse(const char *s, const char **end)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    if (end) *end = s;
    return v;
}

/* Split "a b" into the first word and the rest. */
static inline const char *m_word(const char *s, char *out, uint32_t cap)
{
    uint32_t i = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && i < cap - 1) out[i++] = *s++;
    out[i] = '\0';
    while (*s == ' ') s++;
    return s;
}

/*
 * Build a unit path: m_devpath(buf, "/gpio/", 3) gives "/gpio/3".
 *
 * Written out by hand because the obvious version is a trap. A local
 * `char pre[] = "/gpio/";` compiles to a memcpy from rodata, and a module
 * has no libc to supply one -- the link fails, which is the good case. A
 * pointer to the literal is fine; an array initialised from it is not.
 */
static inline void m_devpath(char *out, const char *dev, uint32_t unit)
{
    uint32_t i = 0;
    while (*dev) out[i++] = *dev++;

    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + unit % 10); unit /= 10; } while (unit);
    while (n > 0) out[i++] = d[--n];

    out[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/*                                                                     */
/* Thin wrappers over setstat, so that code addressing a screen reads   */
/* like code addressing a screen. They work on any console -- the panel */
/* moves its own cursor, a terminal gets the escape sequence -- and the */
/* module never learns which it has.                                    */
/* ------------------------------------------------------------------ */

static inline void m_cursor(const rv9_mod_env_t *env, int path,
                            uint32_t row, uint32_t col)
{
    uint32_t v = (row << 16) | (col & 0xFFFF);
    env->setstat(path, RV9_CON_SS_CURSOR, &v);
}

static inline void m_colour(const rv9_mod_env_t *env, int path,
                            uint32_t fg, uint32_t bg)
{
    uint32_t v = (fg & 0xFF) | ((bg & 0xFF) << 8);
    env->setstat(path, RV9_CON_SS_COLOUR, &v);
}

static inline void m_attr(const rv9_mod_env_t *env, int path, uint32_t flags)
{
    env->setstat(path, RV9_CON_SS_ATTR, &flags);
}

static inline void m_clear(const rv9_mod_env_t *env, int path, uint32_t what)
{
    env->setstat(path, RV9_CON_SS_CLEAR, &what);
}

static inline void m_cursor_on(const rv9_mod_env_t *env, int path, uint32_t on)
{
    env->setstat(path, RV9_CON_SS_CURSOR_ON, &on);
}

/* Rows and columns of whatever is at the far end. Falls back to the
   conventional 80x24 if the call is refused, so a caller always has
   something to lay out against. */
static inline void m_screen(const rv9_mod_env_t *env, int path,
                            uint32_t *rows, uint32_t *cols)
{
    uint32_t v = 0;
    if (env->getstat(path, RV9_CON_GS_SIZE, &v) < 0) v = (24u << 16) | 80u;
    *rows = (v >> 16) & 0xFFFF;
    *cols = v & 0xFFFF;
}

/* Left-aligned in a field of `width`, for table output. */
static inline void m_pad(const rv9_mod_env_t *env, int path, const char *s,
                         uint32_t width)
{
    uint32_t n = m_len(s);
    m_say(env, path, s);
    while (n++ < width) m_say(env, path, " ");
}

static inline void m_numpad(const rv9_mod_env_t *env, int path, int32_t v,
                            uint32_t width)
{
    char buf[12];
    int i = sizeof(buf);
    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;

    buf[--i] = '\0';
    do { buf[--i] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (v < 0) buf[--i] = '-';

    m_pad(env, path, &buf[i], width);
}
