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
 *
 * AND IT SURVIVES A RESET (design section 82)
 *
 * The ring is in memory the boot does not clear, so after a panic or a
 * watchdog it still holds what the run that died was saying. At the next
 * start it is set aside as the previous run's, and health.c writes it to
 * /f0/blackbox when that run ended badly. A power loss clears it; the copy
 * on /f0 does not go with it. A crash's own account -- what faulted, and
 * where -- goes in beside it from ESP-IDF's panic handler, which this file
 * wraps (see the end).
 */
#include "logring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
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

#define BOX_MAGIC 0x52563942u  /* "RV9B" */

/* What a run leaves behind it. Not cleared at boot: see the top. */
typedef struct {
    uint32_t           magic;
    uint32_t           head;          /* where the next byte goes */
    uint32_t           wrapped;       /* whether the oldest byte is at head */
    uint32_t           alive_s;       /* how long the run had been up, last said */
    rv9_logring_panic_t panic;
    char               ring[RING_BYTES];
} box_t;

static __NOINIT_ATTR box_t s_box;
#define s_ring    s_box.ring
#define s_head    s_box.head
#define s_wrapped s_box.wrapped

/* The run before this one's, set aside at the start. */
static char               *s_prev;
static uint32_t            s_prev_len;
static rv9_logring_panic_t s_prev_panic;
static uint32_t            s_prev_alive;

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

/*
 * THE CONSOLE IS NOT ALLOWED IN A CONTROL LOOP.
 *
 * Writing a line is cheap here and expensive on the way out. A line is
 * around a hundred characters, a character at 115200 baud is 87 us, so
 * handing one to the port takes about 8.7 ms -- and it is handed over
 * synchronously, in whatever task did the logging.
 *
 * That included real-time tasks, at the top priority. Worse, it included
 * the one place it matters most: a process that misses its deadline
 * reports why from `finish()` in rv9_proc, which runs on the faulting
 * process's own task and is still at the real-time priority when it logs.
 * So the report cost more than the fault, above the loops it was reporting
 * to -- one miss paying for two more. On the P4 that is what stopped
 * `fastloop`: a 2 ms loop with a 3 ms deadline, losing 8.5 ms to its own
 * startup lines.
 *
 * It was invisible on the C5 only because that board's console is USB
 * Serial/JTAG, where a line costs microseconds. The C5 is the real-time
 * target, and the moment its console is a serial link rather than a bench
 * cable -- 9600 baud is 100 ms a line -- it is far worse there.
 *
 * So a line logged from real-time context goes in the ring and is NOT
 * handed to the port. The next ordinary log line sends it first, which
 * is almost immediately and keeps the order; rv9_logring_flush() is the
 * backstop for when nothing else is logging.
 *
 * WHAT OVERFLOW COSTS, which is less than it sounds
 *
 * Only the port's copy is ever given up. The ring is written before this
 * decision, every time, so `log` over the network still has the line --
 * and the ring is the thing this file exists for. Filling the hand-off
 * takes eight real-time lines inside the ~70 ms it takes to drain eight,
 * which means several loops failing at once; in that state RV-9's own rule
 * applies (see kal_mem.c on the floor): keep the machine running and
 * report the loss, rather than stop a loop to be sure a line reached a
 * cable nobody may be watching. The count is printed when the hand-off
 * next drains, so it is never silent.
 *
 * Raising the baud is not an answer and the arithmetic says so: a line at
 * 460800 is still 2.2 ms, and 2000 us of work plus that does not fit in a
 * 3000 us deadline either.
 */
#define DEFER_LINES 8          /* power of two: the index wraps by masking */

static char     s_defer[DEFER_LINES][LINE_MAX];
static uint8_t  s_defer_head;  /* where the next line goes */
static uint8_t  s_defer_tail;  /* the oldest line waiting */
static uint32_t s_defer_lost;  /* lines the port never got */
static bool     s_flushing;

static uint8_t defer_waiting(void)
{
    return (uint8_t)((s_defer_head - s_defer_tail) & (DEFER_LINES - 1));
}

/* Variadic, because s_onward wants a va_list and what is being sent is a
   line that was formatted long ago -- including its timestamp, which is
   why holding it back cannot make the log lie about when it happened. */
static int onward_fmt(const char *fmt, ...)
{
    if (s_onward == NULL) return 0;

    va_list ap;
    va_start(ap, fmt);
    int n = s_onward(fmt, ap);
    va_end(ap);
    return n;
}

static void defer_put(const char *line)
{
    rv9_critical_enter();
    if (defer_waiting() == DEFER_LINES - 1) {
        s_defer_lost++;                 /* the ring still has it */
    } else {
        /* Bounded: LINE_MAX bytes with interrupts off, which at this clock
           is a few hundred nanoseconds -- shorter than the interrupt it
           could delay, and far shorter than the 8.7 ms it is avoiding. */
        memcpy(s_defer[s_defer_head], line, LINE_MAX);
        s_defer_head = (uint8_t)((s_defer_head + 1) & (DEFER_LINES - 1));
    }
    rv9_critical_exit();
}

void rv9_logring_flush(void)
{
    /* The common case, and it costs one comparison on every log line. */
    if (defer_waiting() == 0 && s_defer_lost == 0) return;
    if (s_flushing) return;             /* s_onward does not log, but still */
    s_flushing = true;

    for (;;) {
        char     line[LINE_MAX];
        uint32_t lost = 0;
        bool     have = false;

        rv9_critical_enter();
        if (defer_waiting() > 0) {
            memcpy(line, s_defer[s_defer_tail], LINE_MAX);
            s_defer_tail = (uint8_t)((s_defer_tail + 1) & (DEFER_LINES - 1));
            have = true;
        } else {
            lost = s_defer_lost;
            s_defer_lost = 0;
        }
        rv9_critical_exit();

        if (have) {
            line[LINE_MAX - 1] = '\0';
            onward_fmt("%s", line);
            continue;
        }

        if (lost > 0) {
            onward_fmt("W rv9-logring: %lu line(s) from real-time context "
                       "went to the ring only; read them with `log`\n",
                       (unsigned long)lost);
        }
        break;
    }

    s_flushing = false;
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
    } else {
        line[0] = '\0';
    }

    if (rv9_rt_in_realtime()) {
        defer_put(line);
        return (n > 0) ? n : 0;
    }

    /* Anything held back goes first, so the order on the cable is the
       order it happened in. */
    rv9_logring_flush();

    return s_onward ? s_onward(fmt, args) : 0;
}

void rv9_logring_start(void)
{
    /* What the last run left, if it is a box and not what power-on leaves. */
    if (s_box.magic == BOX_MAGIC && s_box.head < RING_BYTES) {
        uint32_t held  = s_box.wrapped ? RING_BYTES : s_box.head;
        uint32_t start = s_box.wrapped ? s_box.head : 0;
        s_prev = held ? rv9_alloc(held) : NULL;
        if (s_prev) {
            for (uint32_t i = 0; i < held; i++) s_prev[i] = s_box.ring[(start + i) % RING_BYTES];
            s_prev_len = held;
        }
        s_prev_panic = s_box.panic;
        s_prev_alive = s_box.alive_s;
        s_prev_panic.reason[sizeof(s_prev_panic.reason) - 1] = '\0';
    }
    s_box.magic  = BOX_MAGIC;
    s_box.alive_s = 0;
    memset(&s_box.panic, 0, sizeof(s_box.panic));
    s_head       = 0;
    s_wrapped    = false;
    s_defer_head = 0;
    s_defer_tail = 0;
    s_defer_lost = 0;
    s_onward     = esp_log_set_vprintf(log_vprintf);
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

/* The previous run's log, oldest first, and what its panic said. */
const char *rv9_logring_prev(uint32_t *len, rv9_logring_panic_t *panic)
{
    if (len) *len = s_prev_len;
    if (panic) *panic = s_prev_panic;
    return s_prev;
}

void rv9_logring_alive(uint32_t up_s)
{
    s_box.alive_s = up_s;
}

uint32_t rv9_logring_prev_alive(void)
{
    return s_prev_alive;
}

void rv9_logring_prev_free(void)
{
    rv9_free(s_prev);
    s_prev = NULL;
    s_prev_len = 0;
}

/*
 * ESP-IDF's panic handler, wrapped (main/CMakeLists.txt, --wrap): what
 * faulted and where, into the box, before it prints the same to a serial
 * port nobody may be watching and resets. In the panic's own context, so
 * nothing but stores into memory that is already there.
 */
void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    rv9_logring_panic_t *p = &s_box.panic;
    if (info) {
        const char *r = info->reason ? info->reason : "?";
        size_t i = 0;
        for (; r[i] && i < sizeof(p->reason) - 1; i++) p->reason[i] = r[i];
        p->reason[i] = '\0';
        p->core = (uint32_t)info->core;
        p->pc = (uint32_t)(uintptr_t)info->addr;
        /* An abort -- the task watchdog's, an assert -- is not a CPU
           exception, and its frame's cause and value are not one's: only
           where it was is kept then. */
        const RvExcFrame *f = (const RvExcFrame *)info->frame;
        if (f) {
            p->ra = (uint32_t)f->ra;
            p->sp = (uint32_t)f->sp;
            if (!info->pseudo_excause) {
                p->cause = (uint32_t)f->mcause;
                p->tval  = (uint32_t)f->mtval;
            }
            if (p->pc == 0) p->pc = (uint32_t)f->mepc;
        }
        p->valid = 1;
    }
    __real_esp_panic_handler(info);
}
