/*
 * wnest -- a window that is one tree, driven by the toolkit.
 *
 *     wnest > /w0
 *
 * design.md §5f and §5i, on the board: a toolbar row, a body row holding a
 * list beside a column of buttons, and a status row -- five stacks, nested
 * three deep, laid out from one root. Every touch goes to wsk_feed, which
 * presses, clicks and drags; this program only prints what it was told and
 * repaints the rows it was given. Before §5i it found the widget, pressed it
 * and repainted it by hand -- the toolkit is that code, written once.
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
	{ .kind = WSK_BUTTON, .id = "i0",  .text = "Item 0",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i1",  .text = "Item 1",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i2",  .text = "Item 2",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i3",  .text = "Item 3",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i4",  .text = "Item 4",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i5",  .text = "Item 5",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i6",  .text = "Item 6",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i7",  .text = "Item 7",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i8",  .text = "Item 8",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i9",  .text = "Item 9",  .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i10", .text = "Item 10", .size = 56 },
	{ .kind = WSK_BUTTON, .id = "i11", .text = "Item 11", .size = 56 },
};
static const wsk_widget_t SIDE[] = {
	{ .kind = WSK_BUTTON, .id = "yes", .text = "Yes", .size = 96 },
	{ .kind = WSK_BUTTON, .id = "no",  .text = "No",  .size = 96 },
};
static const wsk_widget_t ST[] = {
	{ .kind = WSK_LABEL, .id = "msg", .text = "tap anything", .weight = 1 },
};

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_tree_t w, k[5];
	wsk_ui_t ui;
	wsk_sink_t s;
	out_t out;
	const wsk_tree_t *in = 0;
	uint32_t size = 0;
	int tp, which = -1, e;

	if (env == NULL || env->abi_version < 13) return 1;
	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wnest: stdout is not a window; send it to /w0\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;

	/* Static panels throughout: the toolkit never writes a widget (§5i). */
	wsk_init(&w, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	w.pad = 0; w.gap = 0; w.font = 24; w.bg = 0x10203au; w.ids = 1;
	w.item = WIN; w.n = 3;
	w.nest = k; w.nests = 5;
	wsk_nest_init(&k[0], &w, WSK_ROW);
	k[0].pad = 6; k[0].gap = 6; k[0].item = TB; k[0].n = 3;
	wsk_nest_init(&k[1], &w, WSK_ROW);
	k[1].pad = 6; k[1].gap = 12; k[1].item = BODY; k[1].n = 2;
	wsk_nest_init(&k[2], &w, WSK_COL);
	k[2].pad = 0; k[2].gap = 4; k[2].item = LIST; k[2].n = 12;
	k[2].overflow = WSK_SCROLL;
	wsk_nest_init(&k[3], &w, WSK_COL);
	k[3].pad = 0; k[3].gap = 12; k[3].item = SIDE; k[3].n = 2;
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
	wsk_ui_init(&ui, &w);

	for (;;) {
		rv9_touch_event_t te[4];
		int n, count;
		uint32_t waiting = 0;

		if (env->signals_take() & RV9_SIG_STOP) break;
		if (env->getstat(tp, RV9_GS_READY, &waiting) == 0 && waiting == 0) {
			env->sleep_ms(8);
			continue;
		}
		n = env->read(tp, te, sizeof te);
		if (n < 0) break;
		count = n / (int)sizeof(rv9_touch_event_t);
		for (int i = 0; i < count; i++) {
			wsk_event_t ev;
			wsk_note_t note[2];
			int y0 = 0, y1 = 0, got;

			ev.kind = (te[i].kind == RV9_TOUCH_DOWN) ? WSK_EV_DOWN
			        : (te[i].kind == RV9_TOUCH_UP)   ? WSK_EV_UP : WSK_EV_MOVE;
			ev.x = (short)te[i].x;
			ev.y = (short)te[i].y;
			ev.d = 0; ev.key = 0; ev.text[0] = '\0'; ev.ms = te[i].at_ms;
			got = wsk_feed(&ui, &ev, note, 2, &y0, &y1);
			for (int j = 0; j < got && j < 2; j++) {
				if (note[j].kind == WSK_NOTE_CLICK) {
					m_say(env, RV9_STDERR, "  click ");
					m_say(env, RV9_STDERR, note[j].in->item[note[j].item].id);
					m_say(env, RV9_STDERR, "\n");
				}
			}
			if (y1 > y0) {
				uint32_t v = ((uint32_t)y0 << 16) | ((uint32_t)y1 & 0xFFFFu);
				env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
				wsk_render_rows(&w, y0, y1, &s, 4096);
			}
		}
	}
	env->close(tp);
	m_say(env, RV9_STDERR, "  the list ended scrolled to ");
	m_num(env, RV9_STDERR, k[2].scroll);
	m_say(env, RV9_STDERR, "\n");
	return 0;
}
