/*
 * wnest -- a window that is one tree, and taps that name the deepest widget.
 *
 *     wnest > /w0
 *
 * design.md §5f, on the board: a toolbar row, a body row holding a list
 * beside a column of buttons, and a status row -- five stacks, nested
 * three deep, laid out from one root. A tap prints the id of the deepest
 * widget under it and the tree it is in, and a button tapped is pressed by
 * repainting only its own rows, found by id at whatever depth it lives.
 *
 * It is also the program tools/evidence/stack measures for nesting: layout,
 * check, hit and render all recurse now, and a recursion's depth is not in
 * any one frame. Built for the P4's 1024x600; on a screen too short for it
 * the tree is refused, by name, which is D10 doing its job.
 *
 * The panels are static and nest by INDEX (`.sub`), never by pointer -- a
 * pointer in rodata is what a module cannot link -- and the one pointer,
 * `nest`, is set at runtime, as `item` always was.
 */
#include "modlib.h"
#include "whisker.h"

static const wsk_widget_t WIN[] = {
	{ .kind = WSK_STACK, .id = "tb",   .sub = 0, .size = 72 },
	{ .kind = WSK_STACK, .id = "body", .sub = 1, .weight = 1 },
	{ .kind = WSK_STACK, .id = "st",   .sub = 4, .size = 40, .fill = 0x203040u },
};
static const wsk_widget_t TB[] = {
	{ .kind = WSK_BUTTON, .id = "back", .text = "Back", .weight = 1 },
	{ .kind = WSK_BUTTON, .id = "stop", .text = "STOP", .weight = 1, .fill = 0x8f2a2au },
	{ .kind = WSK_BUTTON, .id = "go",   .text = "Go",   .weight = 1 },
};
static const wsk_widget_t BODY[] = {
	{ .kind = WSK_STACK, .id = "list",  .sub = 2, .weight = 2 },
	{ .kind = WSK_STACK, .id = "side",  .sub = 3, .weight = 1 },
};
static const wsk_widget_t LIST[] = {
	{ .kind = WSK_BUTTON, .id = "i0", .text = "Item 0", .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i1", .text = "Item 1", .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i2", .text = "Item 2", .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i3", .text = "Item 3", .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i4", .text = "Item 4", .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i5", .text = "Item 5", .size = 56 },
};
static const wsk_widget_t SIDE[] = {
	{ .kind = WSK_BUTTON, .id = "yes", .text = "Yes", .size = 96 },
	{ .kind = WSK_BUTTON, .id = "no",  .text = "No",  .size = 96 },
};
static const wsk_widget_t ST[] = {
	{ .kind = WSK_LABEL, .id = "msg", .text = "tap anything", .weight = 1 },
};

#define PRESSED 0xd08030u

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

/* Repaint the rows of one widget, found by id at any depth. */
static int repaint(const rv9_mod_env_t *env, const wsk_tree_t *w,
                   const char *id, wsk_sink_t *s)
{
	const char *ids[1];
	int y0 = 0, y1 = 0;
	uint32_t v;

	ids[0] = id;
	if (wsk_rows(w, ids, 1, &y0, &y1) != WSK_OK) return 0;
	v = ((uint32_t)y0 << 16) | ((uint32_t)y1 & 0xFFFFu);
	env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
	return wsk_render_clipped(w, ids, 1, s, 4096);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t tb[3], list[6], side[2];
	wsk_tree_t w, k[5];
	wsk_sink_t s;
	out_t out;
	const wsk_tree_t *in = 0;
	uint32_t size = 0;
	int tp, which = -1, e, held = -1;
	wsk_widget_t *held_in = 0;
	unsigned int held_fill = 0;

	if (env == NULL || env->abi_version < 13) return 1;
	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wnest: stdout is not a window; send it to /w0\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;

	wsk_clone(tb, TB, 3);
	wsk_clone(list, LIST, 6);
	wsk_clone(side, SIDE, 2);

	wsk_init(&w, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	w.pad = 0; w.gap = 0; w.font = 24; w.bg = 0x10203au; w.ids = 1;
	w.item = WIN; w.n = 3;
	w.nest = k; w.nests = 5;
	wsk_nest_init(&k[0], &w, WSK_ROW);
	k[0].pad = 6; k[0].gap = 6; k[0].item = tb; k[0].n = 3;
	wsk_nest_init(&k[1], &w, WSK_ROW);
	k[1].pad = 6; k[1].gap = 12; k[1].item = BODY; k[1].n = 2;
	wsk_nest_init(&k[2], &w, WSK_COL);
	k[2].pad = 0; k[2].gap = 4; k[2].item = list; k[2].n = 6;
	k[2].overflow = WSK_SCROLL;
	wsk_nest_init(&k[3], &w, WSK_COL);
	k[3].pad = 0; k[3].gap = 12; k[3].item = side; k[3].n = 2;
	wsk_nest_init(&k[4], &w, WSK_ROW);
	k[4].pad = 4; k[4].font = 20; k[4].ink = 0x90a8c0u; k[4].item = ST; k[4].n = 1;

	e = wsk_check_in(&w, &which, &in);
	if (e != WSK_OK) {
		m_say(env, RV9_STDERR, "wnest: the window was refused, ");
		m_num(env, RV9_STDERR, e);
		m_say(env, RV9_STDERR, " at ");
		m_say(env, RV9_STDERR, (in != 0 && which >= 0) ? in->item[which].id : "a stack");
		m_say(env, RV9_STDERR, "\n");
		return 3;
	}

	out.env = env;
	out.path = RV9_STDOUT;
	s.put = to_path; s.ctx = &out; s.len = 0;
	if (wsk_render(&w, &s, 4096) < 0) return 4;

	tp = env->open("/touch", RV9_MODE_READ);
	if (tp < 0) return 0;                  /* drawn; nothing to tap with */

	for (;;) {
		rv9_touch_event_t ev[4];
		int n, count;
		uint32_t waiting = 0;

		if (env->signals_take() & RV9_SIG_STOP) break;
		if (env->getstat(tp, RV9_GS_READY, &waiting) == 0 && waiting == 0) {
			env->sleep_ms(8);
			continue;
		}
		n = env->read(tp, ev, sizeof ev);
		if (n < 0) break;
		count = n / (int)sizeof(rv9_touch_event_t);
		for (int i = 0; i < count; i++) {
			int x = (int)ev[i].x, y = (int)ev[i].y, hit;
			if (ev[i].kind == RV9_TOUCH_DOWN) {
				hit = wsk_hit_in(&w, x, y, &in);
				m_say(env, RV9_STDERR, "  tap ");
				m_num(env, RV9_STDERR, x);
				m_say(env, RV9_STDERR, ",");
				m_num(env, RV9_STDERR, y);
				if (hit < 0) { m_say(env, RV9_STDERR, " -> nothing\n"); continue; }
				m_say(env, RV9_STDERR, " -> ");
				m_say(env, RV9_STDERR, in->item[hit].id);
				m_say(env, RV9_STDERR, (in->item[hit].kind == WSK_STACK) ? " (a stack)\n" : "\n");
				/* Press it, if it is a button in one of our mutable copies. */
				if (in->item[hit].kind == WSK_BUTTON &&
				    (in == &k[0] || in == &k[2] || in == &k[3])) {
					held_in = (wsk_widget_t *)&in->item[hit];
					held = hit;
					held_fill = held_in->fill;
					held_in->fill = PRESSED;
					repaint(env, &w, held_in->id, &s);
				}
			} else if (ev[i].kind == RV9_TOUCH_UP && held >= 0) {
				held_in->fill = held_fill;
				repaint(env, &w, held_in->id, &s);
				held = -1;
			}
		}
	}
	env->close(tp);
	return 0;
}
