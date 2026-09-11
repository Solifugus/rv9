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
