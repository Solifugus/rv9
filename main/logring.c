/*
 * The log, kept rather than only sent.
 *
 * RV-9 logs a great deal and every word of it went straight out of the
 * USB serial port and was gone. Which meant that the answer to "why did
 * that fail" lived on a wire, and reading it needed a cable, a terminal
 * and physical presence -- on a machine whose entire point is that you
 * work with it over the network.
 *
 * Worse, reaching for the cable is how the evidence gets destroyed: the
 * board resets when the port is opened by the wrong thing, and the log
 * you came to read is the log that no longer exists.
 *
 * So the last few kilobytes are kept here as well as sent. `log` reads
 * them back through sysinfo, like `procs` and `mdir` read theirs.
 *
 * WHY A RING AND NOT A FILE
 *
 * A file would need a volume mounted, which is not true early in boot,
 * and would write to flash on every line, which is both slow and a way to
 * wear the part out. A ring in RAM costs its own size and nothing else,
 * survives everything except a reset, and the thing it is most wanted for
 * -- what happened in the last minute before something went wrong -- is
 * exactly what a ring holds.
 */
#include "logring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "rv9/kal.h"

/*
 * Eight kilobytes, which is around a hundred and fifty lines.
 *
 * Four was the first guess and it held seventy-two -- less than this
 * board's own boot produces, so `log` showed the end of starting up and
 * nothing before it. Sized against what it is for: the tail before a
 * failure, which is worth more than the memory it costs now that there
 * is memory (§48).
 */
#define RING_BYTES 8192

/* One line's worth on the way in. Anything longer is truncated here
   rather than dropped, which is the same bargain every filter makes. */
#define LINE_MAX 256

static char     s_ring[RING_BYTES];
static uint32_t s_head;        /* where the next byte goes */
static bool     s_wrapped;     /* whether the oldest byte is at s_head */
static vprintf_like_t s_onward;

/*
 * Appending is done with interrupts left alone and no lock.
 *
 * The writer is whatever task is logging, and there can be more than one.
 * A lock here would be taken on every log line in the system, including
 * lines written from inside drivers holding their own locks -- which is
 * exactly how a logging facility becomes the thing that deadlocks the
 * machine.
 *
 * So the ring is written without one, and the cost is that two tasks
 * logging at the same instant can interleave their bytes. A garbled line
 * in a ring buffer is a cosmetic fault. A deadlock in the logger is not.
 */
static void ring_put(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        s_ring[s_head] = s[i];
        s_head = (s_head + 1u) % RING_BYTES;
        if (s_head == 0) s_wrapped = true;
    }
}

static int log_vprintf(const char *fmt, va_list args)
{
    char    line[LINE_MAX];
    va_list copy;

    /*
     * The list is walked twice -- once for the ring, once for the port --
     * so it has to be copied. Using it twice without is undefined, and on
     * this architecture it is the kind of undefined that works until an
     * argument happens to land in a register rather than on the stack.
     */
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0) {
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        ring_put(line, n);
    }

    return s_onward ? s_onward(fmt, args) : 0;
}

void rv9_logring_start(void)
{
    s_head    = 0;
    s_wrapped = false;
    s_onward  = esp_log_set_vprintf(log_vprintf);
}

/*
 * Hand back a window of the log, oldest first.
 *
 * `from` is a byte offset into what is currently held, so a reader walks
 * forward by adding what it was given. The offsets shift under a reader
 * that dawdles while the machine is logging -- the oldest bytes fall off
 * the end -- and that is the honest behaviour for a ring: a tool reading
 * a log it cannot keep up with should see the recent past, not a
 * consistent view of an old one.
 */
uint32_t rv9_logring_read(uint32_t from, char *out, uint32_t cap)
{
    if (out == NULL || cap == 0) return 0;

    uint32_t held  = s_wrapped ? RING_BYTES : s_head;
    uint32_t start = s_wrapped ? s_head : 0;

    if (from >= held) return 0;

    uint32_t want = held - from;
    if (want > cap) want = cap;

    for (uint32_t i = 0; i < want; i++) {
        out[i] = s_ring[(start + from + i) % RING_BYTES];
    }
    return want;
}

uint32_t rv9_logring_held(void)
{
    return s_wrapped ? RING_BYTES : s_head;
}
