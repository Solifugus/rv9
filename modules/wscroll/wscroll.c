/*
 * wscroll -- drag a list of ten thousand rows, and time every frame of it.
 *
 *     wscroll > /w0            the pane is the screen below the header
 *     wscroll 112 > /w0        a 112-row pane, which §5e's pricing says
 *                              should hold 30 fps on the P4
 *
 * design.md §5e, on the board. Two things nobody has measured, and the
 * reason this exists rather than a picture:
 *
 *   1. Whether a drag is smooth. tools/evidence/scroll/ DERIVED that a pane
 *      of ~110 rows repaints inside a 30 fps frame and said so in capitals:
 *      "IMPLIED, NOT DEMONSTRATED: nobody has dragged anything on either
 *      panel and timed it." This drags, and times each frame twice -- the
 *      repaint alone, and the finger's report to the last byte written,
 *      which is what a person feels.
 *   2. That a tap through a scrolled pane names the row under it. A tap on
 *      a row selects it and prints which row hit said it was, so a person
 *      can see whether the name matches the label they pressed.
 *
 * WHAT IT SHOWS ABOUT WHISKER, which is the slice. A row is a widget of
 * sixty-odd bytes, so ten thousand of them is more RAM than this process
 * has, and laid out in one stack they would cost the square of the list
 * and run past the row a short can hold. So the program keeps no rows at
 * all: wsk_slice says which few are in view, and they are built on the
 * stack each frame from their index. That is D5 at its plainest -- the
 * list is the caller's, and whisker only ever sees what it draws.
 *
 * The loop reads every event waiting, applies the last position, and draws
 * once: a drag reports a MOVE every 16 ms poll, and drawing each one when
 * the last frame took longer than that would fall further behind the
 * finger with every frame.
 */
#include "modlib.h"
#include "whisker.h"

#define TOTAL     10000       /* rows in the list */
#define ROW       48          /* each row's height: over the 42 a tap wants (§5b) */
#define GAP       2
#define HEAD      48          /* the header's rows; band-aligned */
#define SLOP      16          /* travel under this is a tap, not a drag (§5b's
                                 worst scatter was 21 px, so this errs to tap) */
#define MAX_VIEW  16          /* rows a 600-row screen can show, and then some */

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

/* A row, built from its index. No snprintf in a module, so by hand. */
static void make_row(wsk_widget_t *w, int k, int selected)
{
	char num[8];
	int d = 0, i = 0, v = k;

	for (i = 0; i < WSK_ID_MAX; i++) w->id[i] = '\0';
	for (i = 0; i < WSK_TEXT_MAX; i++) w->text[i] = '\0';
	do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v != 0 && d < 7);

	w->kind = WSK_BUTTON;
	w->size = ROW;
	w->weight = 0;
	w->ink = WSK_INHERIT;
	w->mark = selected ? WSK_MARK_CORNERS : 0;
	w->fill = selected ? 0x8f6a2au : ((k & 1) ? 0x2a6b8fu : 0x24597au);

	w->id[0] = 'r';
	for (i = 0; i < d; i++) w->id[1 + i] = num[d - 1 - i];
	w->text[0] = 'R'; w->text[1] = 'o'; w->text[2] = 'w'; w->text[3] = ' ';
	for (i = 0; i < d; i++) w->text[4 + i] = num[d - 1 - i];
}

typedef struct {
	wsk_doc_t   doc;
	wsk_tree_t  part[2];      /* the header, then the pane */
} scene_t;

static int body(wsk_sink_t *s, void *ctx)
{
	scene_t *c = (scene_t *)ctx;
	int e;

	wsk_open(&c->doc, s);
	if ((e = wsk_part(&c->doc, &c->part[0], s)) != WSK_OK) return e;
	if ((e = wsk_part(&c->doc, &c->part[1], s)) != WSK_OK) return e;
	wsk_shut(s);
	return WSK_OK;
}

/* Append a number or a string to a label; no libc in a module. */
static int put_num(char *t, int at, int v)
{
	char num[8];
	int d = 0;
	do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v != 0 && d < 7);
	while (d > 0 && at < WSK_TEXT_MAX - 1) t[at++] = num[--d];
	t[at] = '\0';
	return at;
}

static int put_str(char *t, int at, const char *s)
{
	while (*s != '\0' && at < WSK_TEXT_MAX - 1) t[at++] = *s++;
	t[at] = '\0';
	return at;
}

typedef struct {
	uint32_t frames;
	uint32_t draw_sum_us, draw_worst_us;      /* the repaint alone */
	uint32_t feel_worst_ms;                   /* the finger's report to the last byte */
	uint32_t over_33;                         /* frames a 30 fps drag would drop */
} drag_t;

/* Microseconds as milliseconds to one decimal: 3.3, not 33/10. */
static void say_tenths(const rv9_mod_env_t *env, uint32_t us)
{
	m_num(env, RV9_STDERR, (int32_t)(us / 1000));
	m_say(env, RV9_STDERR, ".");
	m_num(env, RV9_STDERR, (int32_t)((us / 100) % 10));
}

/*
 * One picture: the slice at `scroll`, built from nothing, and either the
 * whole screen (whole = 1: the first frame, and after a tap or a drag ends,
 * when the header changes) or only the pane's band-snapped rows (a drag
 * frame). Returns the microseconds the repaint took, or a WSK_E_*.
 */
static int draw(const rv9_mod_env_t *env, scene_t *c, wsk_widget_t *view,
                wsk_sink_t *s, int scroll, int selected, int whole, int *first)
{
	int y0 = 0, y1 = c->part[0].h, n, i;
	uint64_t t0;
	uint32_t v;

	n = wsk_slice(&c->part[1], TOTAL, ROW, scroll, first);
	if (n != WSK_OK) return n;
	if (c->part[1].n > MAX_VIEW) return WSK_E_FIT;   /* a pane taller than MAX_VIEW rows */
	for (i = 0; i < c->part[1].n; i++)
		make_row(&view[i], *first + i, *first + i == selected);
	c->part[1].item = view;

	if (whole) {
		char *t = ((wsk_widget_t *)c->part[0].item)->text;
		int at = put_str(t, 0, "row ");
		at = put_num(t, at, *first);
		if (selected >= 0) {
			at = put_str(t, at, "   selected ");
			put_num(t, at, selected);
		} else {
			at = put_str(t, at, " of ");
			put_num(t, at, TOTAL);
		}
	} else {
		wsk_area_rows(&c->part[1], &y0, &y1);
	}
	wsk_doc_init(&c->doc, &c->part[0]);
	c->doc.y0 = (short)y0;
	c->doc.y1 = (short)y1;
	v = whole ? 0 : (((uint32_t)y0 << 16) | ((uint32_t)y1 & 0xFFFFu));
	env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);

	s->len = 0;
	t0 = env->time_us();
	n = wsk_compose(body, c, s, 4096);
	if (n < 0) return n;
	return (int)(env->time_us() - t0);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t head[1], view[MAX_VIEW];
	scene_t c;
	wsk_sink_t s;
	out_t out;
	drag_t g;
	uint32_t size = 0;
	int w, h, pane, tp, i;
	int scroll = 0, max, first = 0, selected = -1;
	int down = 0, down_y = 0, down_x = 0, down_scroll = 0, dragging = 0;

	if (env == NULL || env->abi_version < 13) return 1;

	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wscroll: stdout is not a window; send it to /w0\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;
	w = (int)(size & 0xFFFFu);
	h = (int)(size >> 16);

	pane = h - HEAD;
	if (env->arg != 0 && env->arg[0] != '\0') {
		int v = (int)m_num_parse(env->arg, 0);
		if (v >= ROW && v <= h - HEAD) pane = v;
	}

	tp = env->open("/touch", RV9_MODE_READ);
	if (tp < 0) {
		m_say(env, RV9_STDERR, "wscroll: no /touch on this machine\n");
		return 3;
	}

	for (i = 0; i < WSK_ID_MAX; i++) head[0].id[i] = '\0';
	for (i = 0; i < WSK_TEXT_MAX; i++) head[0].text[i] = '\0';
	head[0].kind = WSK_LABEL;
	head[0].size = HEAD - 8; head[0].weight = 0;
	head[0].fill = WSK_INHERIT; head[0].ink = WSK_INHERIT; head[0].mark = 0;
	head[0].id[0] = 'h';

	wsk_init(&c.part[0], WSK_COL, w, h);
	c.part[0].pad = 4; c.part[0].gap = 0; c.part[0].font = 20;
	c.part[0].bg = 0x0b1420u; c.part[0].ink = 0x90a8c0u;
	wsk_area(&c.part[0], 0, 0, w, HEAD);
	c.part[0].item = head; c.part[0].n = 1;

	wsk_init(&c.part[1], WSK_COL, w, h);
	c.part[1].pad = 0; c.part[1].gap = GAP; c.part[1].font = 20;
	c.part[1].bg = 0x0b1420u; c.part[1].cross_max = 600;
	c.part[1].overflow = WSK_SCROLL;
	wsk_area(&c.part[1], 0, HEAD, w, pane);
	max = wsk_slice_max(&c.part[1], TOTAL, ROW);

	out.env = env;
	out.path = RV9_STDOUT;
	s.put = to_path; s.ctx = &out; s.len = 0;

	g.frames = g.draw_sum_us = g.draw_worst_us = g.feel_worst_ms = g.over_33 = 0;

	m_say(env, RV9_STDERR, "wscroll: ");
	m_num(env, RV9_STDERR, TOTAL);
	m_say(env, RV9_STDERR, " rows in a pane of ");
	m_num(env, RV9_STDERR, pane);
	m_say(env, RV9_STDERR, " -- drag to scroll, tap a row to select it\n");

	if (draw(env, &c, view, &s, scroll, selected, 1, &first) < 0) {
		m_say(env, RV9_STDERR, "wscroll: the first picture was refused\n");
		env->close(tp);
		return 4;
	}

	for (;;) {
		rv9_touch_event_t ev[8];
		int n, count, moved = 0, redraw = 0;
		uint32_t newest = 0;

		if (env->signals_take() & RV9_SIG_STOP) break;
		{
			uint32_t waiting = 0;
			if (env->getstat(tp, RV9_GS_READY, &waiting) == 0 && waiting == 0) {
				env->sleep_ms(4);
				continue;
			}
		}

		/* Every event waiting, then one picture of where they ended. */
		n = env->read(tp, ev, sizeof ev);
		if (n < 0) break;
		count = n / (int)sizeof(rv9_touch_event_t);
		for (i = 0; i < count; i++) {
			int x = (int)ev[i].x, y = (int)ev[i].y;

			if (ev[i].kind == RV9_TOUCH_DOWN) {
				down = 1; dragging = 0;
				down_x = x; down_y = y; down_scroll = scroll;
			} else if (ev[i].kind == RV9_TOUCH_MOVE && down) {
				int dy = down_y - y, to;
				if (!dragging && (dy > SLOP || dy < -SLOP)) dragging = 1;
				if (!dragging) continue;
				to = down_scroll + dy;
				if (to < 0) to = 0;            /* the caller clamps (§5e) */
				if (to > max) to = max;
				if (to != scroll) { scroll = to; moved = 1; newest = ev[i].at_ms; }
			} else if (ev[i].kind == RV9_TOUCH_UP && down) {
				down = 0;
				redraw = 1;                    /* the header says where we are */
				if (dragging) {
					m_say(env, RV9_STDERR, "  drag ended at row ");
					m_num(env, RV9_STDERR, first);
					m_say(env, RV9_STDERR, "\n");
					continue;
				}
				/* A tap. The pane holds the slice that was drawn, so hit
				   answers in slice indices and `first` makes them the
				   list's. Printed beside the row's own label, so a person
				   can check the name against what their finger was on. */
				{
					int k = wsk_hit(&c.part[1], down_x, down_y);
					m_say(env, RV9_STDERR, "  tap ");
					m_num(env, RV9_STDERR, down_x);
					m_say(env, RV9_STDERR, ",");
					m_num(env, RV9_STDERR, down_y);
					if (k >= 0) {
						selected = first + k;
						m_say(env, RV9_STDERR, " -> ");
						m_say(env, RV9_STDERR, view[k].id);
						m_say(env, RV9_STDERR, ", labelled ");
						m_say(env, RV9_STDERR, view[k].text);
						m_say(env, RV9_STDERR, "\n");
					} else {
						m_say(env, RV9_STDERR, " -> nothing\n");
					}
				}
			}
		}

		if (moved) {
			int us = draw(env, &c, view, &s, scroll, selected, 0, &first);
			if (us < 0) break;
			g.frames++;
			g.draw_sum_us += (uint32_t)us;
			if ((uint32_t)us > g.draw_worst_us) g.draw_worst_us = (uint32_t)us;
			if (us > 33333) g.over_33++;
			/* From the finger's own report, not from when this loop got
			   to it: the 16 ms poll, the queue, the read and the repaint
			   are the whole of what a person waits for (wtap's rule). */
			{
				uint32_t feel = m_age_ms(env->time_us(), (uint64_t)newest * 1000u);
				if (feel > g.feel_worst_ms) g.feel_worst_ms = feel;
			}
		}
		if (redraw && draw(env, &c, view, &s, scroll, selected, 1, &first) < 0) break;
	}

	env->close(tp);

	m_say(env, RV9_STDERR, "\nwscroll: ");
	m_num(env, RV9_STDERR, (int32_t)g.frames);
	m_say(env, RV9_STDERR, " drag frames in a ");
	m_num(env, RV9_STDERR, pane);
	m_say(env, RV9_STDERR, "-row pane\n");
	if (g.frames > 0) {
		m_say(env, RV9_STDERR, "  repaint     mean ");
		say_tenths(env, g.draw_sum_us / g.frames);
		m_say(env, RV9_STDERR, " ms, worst ");
		say_tenths(env, g.draw_worst_us);
		m_say(env, RV9_STDERR, " ms\n  over 33 ms  ");
		m_num(env, RV9_STDERR, (int32_t)g.over_33);
		m_say(env, RV9_STDERR, " frame(s) -- what a 30 fps drag would have dropped\n");
		m_say(env, RV9_STDERR, "  finger to pixels, worst ");
		m_num(env, RV9_STDERR, (int32_t)g.feel_worst_ms);
		m_say(env, RV9_STDERR, " ms\n");
	}
	return 0;
}
