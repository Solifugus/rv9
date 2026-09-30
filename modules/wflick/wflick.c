/*
 * wflick -- flick, generated from a widget tree.
 *
 *     wflick > /w0        a panel drawn, a button flashed, and drawn again
 *
 * The same three draws `flick` times, in the same order, for the same reason
 * -- and the point is the comparison. `flick` holds three SVG documents as
 * string literals, written by hand and checked by eye. This one holds a
 * widget tree and generates them, so the question it answers is what that
 * costs: in bytes on the wire, in milliseconds on the board, and in whether
 * the picture is still right.
 *
 *   1  the whole window, once, as the baseline
 *   2  the button's rows only, in its pressed colour
 *   3  the button's rows only, back to normal
 *
 * 2 and 3 together are what a person experiences as one tap. Read the
 * numbers the way flick's header says to: press is the latency, press plus
 * release is the cost.
 *
 * Three things here are whisker's doing rather than a program's, and each is
 * a thing flick had to get right by hand:
 *
 *   - the row range comes from wsk_rows, snapped outward to whole bands,
 *     and the same range is given to RV9_SVG_SS_ROWS and used to size the
 *     backing rect -- one number, so the two cannot disagree. flick is
 *     correct only because it chose band-aligned rows on purpose.
 *   - the panel size comes from getstat rather than from a constant, so the
 *     same binary lays itself out on the C5 and on the P4.
 *   - nothing is buffered. The sink hands each piece to write() as it is
 *     produced, so a 4 KB document never needs 4 KB of a program's memory.
 *
 * WHISKER IS VENDORED. modules/whisker.h is a byte-for-byte copy of
 * ~/development/whisker/src/whisker.h at commit f45edbf, sitting beside
 * modlib.h for the same reason modlib.h sits there: header-only, one copy
 * per module, no ABI to satisfy. To see whether it has drifted:
 *
 *     diff modules/whisker.h ~/development/whisker/src/whisker.h
 *
 * If RV-9 would rather build.conf carried an extra include directory than
 * keep a copy, that is a better answer and whisker has no opinion.
 */
#include "modlib.h"
#include "whisker.h"

/* rodata: the text is inside the struct, so no addresses, so this links.
   A `static const wsk_widget_t *` table would not, and neither would one
   holding `const char *` labels. */
static const wsk_widget_t PANEL[] = {
	{ .kind = WSK_LABEL,  .id = "title",   .text = "tap test" },
	{ .kind = WSK_BUTTON, .id = "confirm", .text = "CONFIRM" },
	{ .kind = WSK_BUTTON, .id = "cancel",  .text = "Cancel" },
};

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

static int clip_rows(const rv9_mod_env_t *env, int y0, int y1)
{
	uint32_t v = ((uint32_t)y0 << 16) | ((uint32_t)y1 & 0xFFFFu);
	return env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
}

static void report(const rv9_mod_env_t *env, const char *what,
                   uint32_t us, int bytes)
{
	m_say(env, RV9_STDERR, what);
	m_num(env, RV9_STDERR, (int32_t)(us / 1000));
	m_say(env, RV9_STDERR, " ms, ");
	m_num(env, RV9_STDERR, (int32_t)bytes);
	m_say(env, RV9_STDERR, " bytes");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t live[3];
	wsk_tree_t t;
	wsk_sink_t s;
	out_t out;
	const char *ids[1];
	uint32_t size = 0;
	uint64_t t0;
	uint32_t full_us, press_us, release_us;
	int full_n, press_n, release_n;
	int y0 = 0, y1 = 0;

	if (env == NULL || env->abi_version < 11) return 1;   /* time_us */

	/*
	 * Is stdout a window? RV9_GS_SIZE cannot answer that on its own -- it is
	 * the *generic* "how big is this thing" code, so RBF answers it with a
	 * file's length. `wflick > /r0/x` passed this check, took 0 for the panel
	 * size, and wrote a 127-byte document laid out on a 0x0 screen.
	 *
	 * So probe the clip first: RV9_SVG_SS_ROWS is svgwin's own code and
	 * nothing else answers it. 0,0 is the documented way to say "all of it",
	 * so the probe changes nothing.
	 */
	if (clip_rows(env, 0, 0) < 0) {
		m_say(env, RV9_STDERR, "wflick: stdout is not a window; send it to /w0\n");
		return 2;
	}

	/*
	 * Pixels, not characters, and asked for rather than assumed: a whisker
	 * that guesses works at 320x172 and clips the wrong strip at 1024x600.
	 */
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;

	wsk_clone(live, PANEL, 3);
	wsk_init(&t, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	t.pad = 4;
	t.gap = 4;
	t.bg = 0x10203au;                      /* the navy flick uses */
	t.item = live;
	t.n = 3;

	out.env = env;
	out.path = RV9_STDOUT;

	s.put = to_path; s.ctx = &out; s.len = 0;
	t0 = env->time_us();
	full_n = wsk_render(&t, &s, 4096);
	full_us = (uint32_t)(env->time_us() - t0);
	if (full_n < 0) {
		m_say(env, RV9_STDERR, "wflick: the panel does not fit in 4096 bytes\n");
		return 3;
	}

	ids[0] = "confirm";
	if (wsk_rows(&t, ids, 1, &y0, &y1) != WSK_OK) return 4;

	/* The probe above already established that this window clips. */
	clip_rows(env, y0, y1);

	live[1].fill = 0x8fd0ffu;              /* pale, like flick's pressed */
	live[1].ink  = 0x112233u;              /* dark on it, like flick's */
	live[1].mark = WSK_MARK_CORNERS;       /* and a second, geometric signal */
	s.put = to_path; s.ctx = &out; s.len = 0;
	t0 = env->time_us();
	press_n = wsk_render_clipped(&t, ids, 1, &s, 4096);
	press_us = (uint32_t)(env->time_us() - t0);

	env->sleep_ms(60);                     /* long enough for an eye */

	clip_rows(env, y0, y1);                /* the clip is one-shot */
	live[1].fill = WSK_INHERIT;
	live[1].ink  = WSK_INHERIT;
	live[1].mark = 0;
	s.put = to_path; s.ctx = &out; s.len = 0;
	t0 = env->time_us();
	release_n = wsk_render_clipped(&t, ids, 1, &s, 4096);
	release_us = (uint32_t)(env->time_us() - t0);

	/* To stderr, which is not the window: writing the answer into the
	   picture would redraw the thing being measured. */
	report(env, "wflick: full ", full_us, full_n);
	report(env, ", press ", press_us, press_n);
	report(env, ", release ", release_us, release_n);
	m_say(env, RV9_STDERR, " (press is the latency; press+release is the cost)\n");
	m_say(env, RV9_STDERR, "wflick: clipped rows ");
	m_num(env, RV9_STDERR, y0);
	m_say(env, RV9_STDERR, "..");
	m_num(env, RV9_STDERR, y1);
	m_say(env, RV9_STDERR, " of ");
	m_num(env, RV9_STDERR, (int32_t)(size >> 16));
	m_say(env, RV9_STDERR, "\n");
	return 0;
}
