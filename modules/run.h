/*
 * What `on` and execd say to each other over /peer/<name>/run, once the
 * caller has proved who it is (peerfm.c). Frames, both ways:
 *
 *     kind (1 byte)   length (2 bytes, little-endian)   that many bytes
 *
 * The caller sends RUN_ASK first, "listen" or "sort -r", then RUN_IN with
 * what it reads and RUN_EOF when its input ends. The peer sends RUN_OUT
 * and RUN_ERR as the command writes them, and last RUN_EXIT (its status
 * and fault, four bytes each) or RUN_REFUSED (the fork's error) when it
 * never started. Killing the caller hangs up, and execd kills the command.
 *
 * Framed rather than a bare stream as rshd's is, because there are three
 * things to carry back and one connection to carry them in: a status
 * cannot be told from output, nor standard error from standard output.
 */
#pragma once

#define RUN_ASK      'q'
#define RUN_IN       'i'
#define RUN_EOF      'z'
#define RUN_OUT      'o'
#define RUN_ERR      'e'
#define RUN_EXIT     'x'
#define RUN_REFUSED  'r'

#define RUN_FRAME_MAX 512

/* peerfm.c's, which a module cannot include: rv9/net.h's NOWAIT, and who
   called. */
#define RUN_SS_NOWAIT  (RV9_SS_DRIVER_BASE + 5)
#define RUN_GS_CALLER  (RV9_SS_DRIVER_BASE + 32)

static inline int run_send(const rv9_mod_env_t *env, int path, char kind,
                    const void *data, uint32_t len)
{
    uint8_t h[3] = { (uint8_t)kind, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    if (env->write(path, h, 3) != 3) return -1;
    if (len && env->write(path, data, len) != (int)len) return -1;
    return 0;
}

static inline void run_put32(uint8_t *b, int32_t v)
{
    uint32_t u = (uint32_t)v;
    b[0] = (uint8_t)u; b[1] = (uint8_t)(u >> 8); b[2] = (uint8_t)(u >> 16); b[3] = (uint8_t)(u >> 24);
}

static inline int32_t run_get32(const uint8_t *b)
{
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                     ((uint32_t)b[3] << 24));
}

/*
 * Frames arrive in whatever pieces the network makes. `buf` holds `*have`
 * bytes; each whole frame at its front goes to `each`, which returns
 * nonzero to stop, and what is left is moved to the front. Returns what
 * `each` stopped with, or 0.
 */
typedef int (*run_frame_fn)(const rv9_mod_env_t *env, void *ctx, char kind,
                            const uint8_t *data, uint32_t len);

static inline int run_frames(const rv9_mod_env_t *env, void *ctx, uint8_t *buf, uint32_t *have,
                      run_frame_fn each)
{
    uint32_t at = 0;
    int stop = 0;
    while (!stop && *have - at >= 3) {
        uint32_t len = (uint32_t)buf[at + 1] | ((uint32_t)buf[at + 2] << 8);
        if (len > RUN_FRAME_MAX) return -1;
        if (*have - at < 3 + len) break;
        stop = each(env, ctx, (char)buf[at], buf + at + 3, len);
        at += 3 + len;
    }
    for (uint32_t i = at; i < *have; i++) buf[i - at] = buf[i];
    *have -= at;
    return stop;
}
