/*
 * wtap -- the first thing on this board you can actually press.
 *
 *     wtap > /w0          until stopped
 *     wtap 30 > /w0       for thirty seconds
 *
 * It joins the two halves that have existed separately until now. `/w0`
 * draws and `/touch` reports fingers, and nothing in RV-9 has ever put them
 * together: `flick` and `wflick` draw a button and flash it on a timer,
 * which measures the drawing and proves nothing about the pressing.
 *
 * WHAT WHISKER DOES HERE, which is the whole reason this is short.
 *
 *   wsk_hit(tree, x, y)   which widget is under the finger
 *   wsk_rows(tree, ids)   which DEVICE rows that widget occupies
 *   wsk_render_clipped    repaint exactly those rows
 *
 * A tap arrives from /touch in panel pixels, and whisker lays out in panel
 * pixels because wsk_init is given the device size -- so the finger and the
 * layout are already in the same units and there is no conversion to get
 * wrong. That is not a happy accident, it is why whisker insists on being
 * told the device rather than choosing its own coordinate space.
 *
 * It is worth seeing what the alternative costs. `flick` holds its documents
 * as hand-written SVG with viewBox="0 0 320 172" -- the C5's panel -- and
 * clips with those same numbers. On the C5 the viewBox IS the panel so the
 * two agree and nobody notices. On this 1024x600 panel the clip is in panel
 * rows and the document is in its own units, so `flick` repaints rows 96..144
 * of 600 while its button is drawn at panel rows 335..502. It flashes a strip
 * of empty background and reports the time taken, which is how a hand-written
 * document carries the panel it was written for.
 *
 * THE PRESSED LOOK is a corner mark rather than a colour change, because §2.4
 * says the substrate is flat: no shadow, no gradient, no opacity. Whisker has
 * no state and no event model -- deliberately -- so the pressed-ness lives
 * here, in the caller's copy of the tree, which is the only place that knows
 * a finger is down.
 */
#include "modlib.h"
#include "whisker.h"

/* rodata: the text is inside the struct, so no addresses, so this links.
   See the same note in wflick. */
/*
 * The button takes the leftover rows -- .weight = 1 -- and that is not
 * cosmetic. With three fixed-size widgets whisker stacks them from the top
 * at their natural heights, which puts a 64-row button in the top tenth of
 * a 600-row panel and leaves nine tenths of inviting navy that is not the
 * button. The first version of this did exactly that, and the first person
 * to try it pressed the middle of the screen and nothing happened. A demo
 * whose whole purpose is to be pressed should be mostly button.
 */
static const wsk_widget_t PANEL[] = {
	{ .kind = WSK_LABEL,  .id = "title",   .text = "press the big box" },
	{ .kind = WSK_BUTTON, .id = "confirm", .text = "CONFIRM", .weight = 1 },
	{ .kind = WSK_LABEL,  .id = "count",   .text = "waiting" },
};

#define N_ITEM  3

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

/* The button's rows, repainted and nothing else. Returns the microseconds
   it took, or 0 if whisker refused. */
static uint32_t repaint_button(const rv9_mod_env_t *env, wsk_tree_t *t,
                               wsk_sink_t *s, out_t *out)
{
	const char *ids[1] = { "confirm" };
	int y0 = 0, y1 = 0;
	uint64_t t0;

	if (wsk_rows(t, ids, 1, &y0, &y1) != WSK_OK) return 0;
	if (clip_rows(env, y0, y1) < 0) return 0;

	s->put = to_path; s->ctx = out; s->len = 0;
	t0 = env->time_us();

	/*
	 * 4096, the figure wflick uses, and the return value is checked. A
	 * budget too small is refused rather than truncated -- which is the
	 * right behaviour and indistinguishable from a dead button if nobody
	 * looks. This one was 2048 and unchecked.
	 */
	int n = wsk_render_clipped(t, ids, 1, s, 4096);
	if (n < 0) {
		m_say(env, RV9_STDERR, "wtap: the repaint was refused: ");
		m_num(env, RV9_STDERR, (int32_t)n);
		m_say(env, RV9_STDERR, "\n");
		return 0;
	}
	return (uint32_t)(env->time_us() - t0);
}

typedef struct {
	uint32_t taps;
	uint32_t moves;
	uint32_t worst_ms;      /* finger reported to pixels changed */
} wtap_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t live[N_ITEM];
	wsk_tree_t t;
	wsk_sink_t s;
	out_t out;
	uint32_t size = 0;
	int tp;
	int down_on = WSK_E_NOTFOUND;   /* which widget the finger went down on */

	if (env == NULL || env->abi_version < 13) return 1;

	wtap_statics_t *st = (wtap_statics_t *)env->statics;
	if (st == NULL || env->statics_size < sizeof(*st)) return 2;
	st->taps = st->moves = st->worst_ms = 0;

	uint32_t secs = 0;
	if (env->arg != NULL) secs = m_num_parse(env->arg, 0);

	/* Is stdout a window? The clip is svgwin's own setstat and nothing else
	   answers it, so this is the probe wflick uses, for its reasons. */
	if (clip_rows(env, 0, 0) < 0) {
		m_say(env, RV9_STDERR, "wtap: stdout is not a window; send it to /w0\n");
		return 3;
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 3;

	tp = env->open("/touch", RV9_MODE_READ);
	if (tp < 0) {
		m_say(env, RV9_STDERR, "wtap: no /touch on this machine\n");
		return 4;
	}

	wsk_clone(live, PANEL, N_ITEM);
	wsk_init(&t, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	t.pad = 4;
	t.gap = 4;
	t.bg = 0x10203au;
	t.item = live;
	t.n = N_ITEM;

	out.env = env;
	out.path = RV9_STDOUT;
	s.put = to_path; s.ctx = &out; s.len = 0;

	if (wsk_render(&t, &s, 4096) < 0) {
		m_say(env, RV9_STDERR, "wtap: the panel does not fit in 4096 bytes\n");
		env->close(tp);
		return 5;
	}

	{
		const char *ids[1] = { "confirm" };
		int by0 = 0, by1 = 0;
		if (wsk_rows(&t, ids, 1, &by0, &by1) == WSK_OK) {
			m_say(env, RV9_STDERR, "wtap: the button is device rows ");
			m_num(env, RV9_STDERR, (int32_t)by0);
			m_say(env, RV9_STDERR, "..");
			m_num(env, RV9_STDERR, (int32_t)by1);
			m_say(env, RV9_STDERR, " of ");
			m_num(env, RV9_STDERR, (int32_t)(size >> 16));
			m_say(env, RV9_STDERR, "\n");
		}
	}
	m_say(env, RV9_STDERR, "wtap: press the button on the glass\n");

	uint64_t until = secs ? env->time_us() + (uint64_t)secs * 1000000u : 0;

	for (;;) {
		if (until && env->time_us() >= until) break;

		rv9_touch_event_t ev[4];
		int n = env->read(tp, ev, sizeof(ev));
		if (n < 0) break;

		int count = n / (int)sizeof(rv9_touch_event_t);
		for (int i = 0; i < count; i++) {
			int who = wsk_hit(&t, (int)ev[i].x, (int)ev[i].y);

			if (ev[i].kind == RV9_TOUCH_DOWN) {
				/* Where the finger landed and what was there. A run that
				   appears to do nothing should be able to say whether the
				   touch arrived, whether it hit, or whether the repaint
				   was the part that failed. */
				m_say(env, RV9_STDERR, "wtap: down at ");
				m_num(env, RV9_STDERR, (int32_t)ev[i].x);
				m_say(env, RV9_STDERR, ",");
				m_num(env, RV9_STDERR, (int32_t)ev[i].y);
				m_say(env, RV9_STDERR, " -> widget ");
				m_num(env, RV9_STDERR, (int32_t)who);
				m_say(env, RV9_STDERR, "\n");

				down_on = who;
				if (who >= 0 && live[who].kind == WSK_BUTTON) {
					live[who].mark = WSK_MARK_CORNERS;
					repaint_button(env, &t, &s, &out);

					/*
					 * From the event's own timestamp, not from when this
					 * loop got round to it. What a finger experiences is
					 * the whole path -- the controller's 16 ms poll, the
					 * queue, this read, the hit test and the repaint --
					 * and starting the clock here would flatter it by
					 * exactly the part that is hardest to fix.
					 *
					 * m_age_ms because a module may not divide 64 bits;
					 * at_ms is milliseconds, hence the multiply.
					 */
					uint32_t whole = m_age_ms(env->time_us(),
					                          (uint64_t)ev[i].at_ms * 1000u);
					if (whole > st->worst_ms) st->worst_ms = whole;
				}

			} else if (ev[i].kind == RV9_TOUCH_MOVE) {
				st->moves++;
				/* A finger that slides off a held button releases it, which
				   is what every button everywhere does and costs one more
				   repaint to show. */
				if (down_on >= 0 && who != down_on &&
				    live[down_on].mark == WSK_MARK_CORNERS) {
					live[down_on].mark = 0;
					repaint_button(env, &t, &s, &out);
				}

			} else if (ev[i].kind == RV9_TOUCH_UP) {
				if (down_on >= 0 && live[down_on].kind == WSK_BUTTON) {
					bool fired = (live[down_on].mark == WSK_MARK_CORNERS) &&
					             (who == down_on);
					live[down_on].mark = 0;
					repaint_button(env, &t, &s, &out);
					if (fired) st->taps++;
				}
				down_on = WSK_E_NOTFOUND;
			}
		}
	}

	env->close(tp);

	m_say(env, RV9_STDERR, "wtap: ");
	m_num(env, RV9_STDERR, (int32_t)st->taps);
	m_say(env, RV9_STDERR, " tap(s), ");
	m_num(env, RV9_STDERR, (int32_t)st->moves);
	m_say(env, RV9_STDERR, " move(s), worst finger-to-pixels ");
	m_num(env, RV9_STDERR, (int32_t)st->worst_ms);
	m_say(env, RV9_STDERR, " ms\n");
	return 0;
}
