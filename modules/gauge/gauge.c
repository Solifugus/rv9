/*
 * gauge -- a live telemetry display, and a measurement of what /w0 can do.
 *
 *   gauge          draw until interrupted, reporting frames per second
 *   gauge 200      draw 200 frames and stop
 *
 * A chart drawn once is a report. This is the other thing a small panel on
 * a machine is actually for: a number that moves, watched while the machine
 * runs. So it redraws continuously and says how fast it managed, because
 * the useful question about a telemetry display is not whether it is
 * pretty but whether it keeps up.
 *
 * The metric is a real-time task's *lateness* -- how far after its due
 * moment it actually ran -- because that is the number an autonomous
 * machine wants on its panel. Not that the loop is working: by how much
 * it is missing. Overruns and the worst jitter since it started sit
 * alongside, since a trace shows the shape and a number shows the damage.
 *
 * With no real-time task running it falls back to the die temperature,
 * which is honest but, in a library, a flat line.
 *
 *   rt control 20000     a 50 Hz control loop to watch
 *   gauge                watch it
 */
#include "modlib.h"

#define HIST    60          /* samples across the strip chart */
#define SVGMAX  3600

typedef struct {
    char     svg[SVGMAX];
    uint32_t n;
    int16_t  hist[HIST];
    uint32_t count;
} gauge_t;

static void put(gauge_t *g, const char *s)
{
    while (*s && g->n < SVGMAX - 1) g->svg[g->n++] = *s++;
}

static void putn(gauge_t *g, int32_t v)
{
    char tmp[12];
    int i = 0;
    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;

    if (v < 0 && g->n < SVGMAX - 1) g->svg[g->n++] = '-';
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i > 0 && g->n < SVGMAX - 1) g->svg[g->n++] = tmp[--i];
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 12) return -1;

    gauge_t *g = (gauge_t *)env->statics;
    if (g == NULL || env->statics_size < sizeof(*g)) return -2;

    uint32_t want = 0;
    if (env->arg && env->arg[0]) want = m_num_parse(env->arg, 0);

    int w = env->open("/w0", RV9_MODE_WRITE);
    if (w < 0) { m_say(env, RV9_STDOUT, "/w0: cannot open\n"); return -3; }

    /*
     * Something to watch.
     *
     * A monitor should not normally start what it monitors, but the shell
     * waits for a foreground command and `rt` waits for its child, so
     * there is no way to have both running from one console. Forking the
     * loop here makes the demonstration one command, and only happens when
     * nothing real-time is running already.
     */
    rv9_sys_rt_t probe[4];
    if (env->sysinfo(RV9_SYS_RT, probe, sizeof(probe)) <= 0) {
        int rp = env->fork_rt("control", 20000, "20000");
        if (rp >= 0) {
            m_say(env, RV9_STDOUT, "gauge: started a 50 Hz control loop to watch\n");
        }
    }

    int t = env->open("/tsens/0", RV9_MODE_READ);

    uint64_t t0 = env->time_us();
    uint32_t frames = 0;

    for (;;) {
        /*
         * A real-time task's own report, read from outside it. rt_stats
         * would only ever describe this process, which draws pictures and
         * has no deadlines worth watching.
         */
        rv9_sys_rt_t rt[4];
        int nrt = env->sysinfo(RV9_SYS_RT, rt, sizeof(rt));

        int32_t value = 0, scale_hi = 0;
        const char *label = "die temp";
        const char *unit  = " C";

        if (nrt > 0) {
            value    = (int32_t)rt[0].last_exec_us;
            scale_hi = (int32_t)rt[0].max_exec_us;
            label    = rt[0].event_driven ? "exec us (event)" : "exec us";
            unit     = " us";
        } else if (t >= 0) {
            env->read(t, &value, sizeof(value));
        }

        rv9_sys_mem_t mem;
        int32_t heap = 0;
        if (env->sysinfo(RV9_SYS_MEM, &mem, sizeof(mem)) > 0) {
            heap = (int32_t)(mem.heap_free / 1024);
        }

        for (uint32_t i = 0; i + 1 < HIST; i++) g->hist[i] = g->hist[i + 1];
        g->hist[HIST - 1] = (int16_t)((value > 32000) ? 32000 : value);
        if (g->count < HIST) g->count++;

        /*
         * Anchored at zero, and never magnified below a floor.
         *
         * Scaling to the range on screen was the obvious way to show
         * texture and it is how a telemetry display comes to lie. This
         * loop executes in one to two microseconds, so a one-microsecond
         * flicker -- the last bit of a microsecond counter -- was stretched
         * across the whole plot and looked exactly like a heartbeat. The
         * numbers underneath were right the entire time; the shape, which
         * is what a glance actually reads, was invented.
         *
         * A duration has a meaningful zero, so the axis starts there: the
         * same reason a truncated bar chart is dishonest. The top follows
         * what has been seen, with headroom, but never drops below a floor
         * that makes a trivial signal look trivial.
         */
        int32_t lo = 0, top = 0;
        for (uint32_t i = 0; i < g->count; i++) {
            int32_t v = g->hist[HIST - g->count + i];
            if (v > top) top = v;
        }
        if (scale_hi > top) top = scale_hi;      /* the worst ever seen */

        top += top / 4;                          /* headroom */
        if (nrt > 0 && top < 20) top = 20;       /* microseconds */
        if (nrt <= 0) { lo = 20; if (top < 60) top = 60; }   /* degrees */
        if (top <= lo) top = lo + 1;

        /* Build the frame. */
        g->n = 0;
        put(g, "<svg viewBox=\"0 0 320 172\">");
        put(g, "<rect x='0' y='0' width='320' height='172' fill='#0b1016'/>");

        put(g, "<g stroke='#1d2a36' stroke-width='1' fill='none'>"
               "<path d='M8 40 H312 M8 75 H312 M8 110 H312'/></g>");

        /* The trace: one point per sample, oldest at the left. */
        put(g, "<path fill='none' stroke='#48c9a9' stroke-width='2' d='");
        for (uint32_t i = 0; i < g->count; i++) {
            uint32_t idx = HIST - g->count + i;
            int32_t x = 8 + (int32_t)(i * 304 / (HIST - 1));

            int32_t v = g->hist[idx];
            if (v < lo)  v = lo;
            if (v > top) v = top;

            int32_t y = 140 - (v - lo) * 110 / (top - lo);
            put(g, (i == 0) ? "M" : "L");
            putn(g, x); put(g, " "); putn(g, y); put(g, " ");
        }
        put(g, "'/>");

        put(g, "<text x='8' y='20' fill='#7fa8c9' font-size='12'>");
        put(g, label);
        put(g, "</text>");

        put(g, "<text x='312' y='20' fill='#e8f0f5' font-size='16'"
               " text-anchor='end'>");
        putn(g, g->hist[HIST - 1]);
        put(g, unit);
        put(g, "</text>");

        /* The damage, not the shape: worst lateness and anything missed. */
        if (nrt > 0) {
            put(g, "<text x='8' y='36' fill='#d98032' font-size='11'>late ");
            putn(g, (int32_t)rt[0].max_jitter_us);
            put(g, "us  over ");
            putn(g, (int32_t)rt[0].overruns);
            put(g, "  n ");
            putn(g, (int32_t)rt[0].activations);
            put(g, "</text>");
        }

        put(g, "<text x='312' y='40' fill='#3f5464' font-size='10'"
               " text-anchor='end'>");
        putn(g, top);
        put(g, "</text>");
        put(g, "<text x='312' y='144' fill='#3f5464' font-size='10'"
               " text-anchor='end'>");
        putn(g, lo);
        put(g, "</text>");

        put(g, "<text x='8' y='166' fill='#5a7183' font-size='11'>heap ");
        putn(g, heap);
        put(g, "k</text>");

        put(g, "<text x='312' y='166' fill='#5a7183' font-size='11'"
               " text-anchor='end'>");
        putn(g, (int32_t)frames);
        put(g, " frames</text>");
        put(g, "</svg>");

        env->write(w, g->svg, g->n);
        frames++;

        if (want && frames >= want) break;
        if (env->signals_take() & RV9_SIG_STOP) break;
    }

    /* 32-bit throughout: a module has no libc, so a 64-bit divide is an
       undefined __udivdi3 rather than an instruction. A duration fits in
       32 bits of microseconds for seventy minutes, which is longer than
       any run of this. */
    uint32_t us = (uint32_t)(env->time_us() - t0);
    uint32_t ms = us / 1000u;
    if (ms == 0) ms = 1;
    if (frames == 0) frames = 1;

    if (t >= 0) env->close(t);
    env->close(w);

    m_say(env, RV9_STDOUT, "drew ");
    m_num(env, RV9_STDOUT, (int32_t)frames);
    m_say(env, RV9_STDOUT, " frames in ");
    m_num(env, RV9_STDOUT, (int32_t)ms);
    m_say(env, RV9_STDOUT, " ms -- ");
    m_num(env, RV9_STDOUT, (int32_t)(frames * 1000u / ms));
    m_say(env, RV9_STDOUT, " fps, ");
    m_num(env, RV9_STDOUT, (int32_t)(ms / frames));
    m_say(env, RV9_STDOUT, " ms a frame\n");

    /* Said out loud, because the panel cannot be read from here and a
       fallback to the thermometer looks exactly like a flat control loop. */
    rv9_sys_rt_t rt[4];
    int nrt = env->sysinfo(RV9_SYS_RT, rt, sizeof(rt));
    m_say(env, RV9_STDOUT, "watched ");
    m_num(env, RV9_STDOUT, (int32_t)(nrt > 0 ? nrt : 0));
    m_say(env, RV9_STDOUT, " real-time task(s)\n");

    /* The range the trace was drawn against, so that a dramatic-looking
       plot can be told apart from a dramatic signal. An auto-scaled axis
       will make anything look eventful; the numbers say whether it was. */
    int32_t seen_lo = 0x7FFFFFF, seen_hi = -0x7FFFFFF;
    for (uint32_t i = 0; i < g->count; i++) {
        int32_t v = g->hist[HIST - g->count + i];
        if (v < seen_lo) seen_lo = v;
        if (v > seen_hi) seen_hi = v;
    }
    if (g->count > 0) {
        m_say(env, RV9_STDOUT, "plotted ");
        m_num(env, RV9_STDOUT, seen_lo);
        m_say(env, RV9_STDOUT, "..");
        m_num(env, RV9_STDOUT, seen_hi);
        if (nrt > 0) {
            m_say(env, RV9_STDOUT, " us execute, worst late ");
            m_num(env, RV9_STDOUT, (int32_t)rt[0].max_jitter_us);
            m_say(env, RV9_STDOUT, " us, overruns ");
            m_num(env, RV9_STDOUT, (int32_t)rt[0].overruns);
        }
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
