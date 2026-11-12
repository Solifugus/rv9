/*
 * wkeys -- type with a finger: a field and an on-screen keyboard.
 *
 *     wkeys > /w0
 *
 * design.md §5j, and §9.5's first real program for the P4: a board with a
 * touch panel and no keyboard can now be typed on. It is built on the
 * toolkit, so it is mostly a keyboard layout:
 *
 *   - every touch goes to wsk_feed, which presses keys, focuses the field
 *     when it is tapped and puts the caret where the finger landed;
 *   - a key's CLICK is turned into what a keyboard would have sent -- a TEXT
 *     event for a character, a KEY event for Left, Right, Del and Enter --
 *     and fed straight back to wsk_feed, which edits the field. Tapping a key
 *     does not take focus from the field (§5j rule 2), which is the whole of
 *     why that rule is there.
 *
 * Enter prints the field to stderr; Clear empties it. The keys are static
 * panels and are never written; the field's text is in a buffer this program
 * owns (§5j), and that is the only text anything changes.
 *
 * SHIFT, as a phone's: tap it and the letters are capitals for the next
 * letter only, then lower case again; its label says which (Shift / SHIFT).
 * Added 2026-11-11 after the first person to type on it asked where the
 * capitals were.
 *
 * TWO LAYERS, as a phone's keyboard has, and for the reason a phone has
 * them: all forty-three keys on one screen came to 4,298 bytes of document
 * against /w0's 4,096 (SRC_MAX). Letters, and a 123 key that swaps three rows
 * for digits and symbols -- 34 keys on the glass at a time. The swap is a
 * row's `item` pointed at another static array: trees are the program's and
 * writable, widgets are never touched.
 */
#include "modlib.h"

/* A touch panel's worth (D20): fields, marks, scrolling, nesting and the
   toolkit -- no wrap, no text items, which a keyboard never uses. */
#define WSK_PANEL 1
#include "whisker.h"

#define K(c) { .kind = WSK_BUTTON, .text = c, .weight = 1 }

static const wsk_widget_t WIN[] = {
	{ .kind = WSK_STACK, .id = "top",  .sub = 0, .size = 88 },
	{ .kind = WSK_STACK, .id = "keys", .sub = 1, .weight = 1 },
};
static const wsk_widget_t TOP[] = {
	{ .kind = WSK_FIELD,  .id = "text",  .sub = 0, .weight = 1 },
	{ .kind = WSK_BUTTON, .id = "clear", .text = "Clear", .size = 140 },
};
static const wsk_widget_t KEYS[] = {
	{ .kind = WSK_STACK, .sub = 2, .weight = 1 },
	{ .kind = WSK_STACK, .sub = 3, .weight = 1 },
	{ .kind = WSK_STACK, .sub = 4, .weight = 1 },
	{ .kind = WSK_STACK, .sub = 5, .weight = 1 },
};
/* letters */
static const wsk_widget_t L1[] = { K("q"), K("w"), K("e"), K("r"), K("t"),
                                   K("y"), K("u"), K("i"), K("o"), K("p") };
static const wsk_widget_t L2[] = { K("a"), K("s"), K("d"), K("f"), K("g"),
                                   K("h"), K("j"), K("k"), K("l") };
static const wsk_widget_t L3[] = { K("Shift"), K("z"), K("x"), K("c"), K("v"),
                                   K("b"), K("n"), K("m"), K(","), K(".") };
/* capitals, for one letter after Shift */
static const wsk_widget_t U1[] = { K("Q"), K("W"), K("E"), K("R"), K("T"),
                                   K("Y"), K("U"), K("I"), K("O"), K("P") };
static const wsk_widget_t U2[] = { K("A"), K("S"), K("D"), K("F"), K("G"),
                                   K("H"), K("J"), K("K"), K("L") };
static const wsk_widget_t U3[] = { K("SHIFT"), K("Z"), K("X"), K("C"), K("V"),
                                   K("B"), K("N"), K("M"), K(","), K(".") };
/* digits and symbols -- none the window cannot draw (§2.4: no < & >) */
static const wsk_widget_t N1[] = { K("1"), K("2"), K("3"), K("4"), K("5"),
                                   K("6"), K("7"), K("8"), K("9"), K("0") };
static const wsk_widget_t N2[] = { K("-"), K("/"), K(":"), K(";"), K("("),
                                   K(")"), K("$"), K("@"), K("?") };
static const wsk_widget_t N3[] = { K("!"), K("'"), K("#"), K("%"), K("*"),
                                   K("+"), K("="), K("_"), K(".") };
#define BOTTOM(layer) {                                              \
	{ .kind = WSK_BUTTON, .text = layer,   .weight = 2 },            \
	{ .kind = WSK_BUTTON, .text = "Left",  .weight = 2 },            \
	{ .kind = WSK_BUTTON, .text = "space", .weight = 5 },            \
	{ .kind = WSK_BUTTON, .text = "Right", .weight = 2 },            \
	{ .kind = WSK_BUTTON, .text = "Del",   .weight = 2 },            \
	{ .kind = WSK_BUTTON, .text = "Enter", .weight = 2 } }
static const wsk_widget_t B_LETTERS[] = BOTTOM("123");
static const wsk_widget_t B_SYMBOLS[] = BOTTOM("abc");

enum { LOWER, UPPER, SYMBOLS };

/* Point the rows at a layer. */
static void layer(wsk_tree_t *k, int which)
{
	k[2].item = (which == SYMBOLS) ? N1 : (which == UPPER) ? U1 : L1; k[2].n = 10;
	k[3].item = (which == SYMBOLS) ? N2 : (which == UPPER) ? U2 : L2; k[3].n = 9;
	k[4].item = (which == SYMBOLS) ? N3 : (which == UPPER) ? U3 : L3;
	k[4].n = (which == SYMBOLS) ? 9 : 10;
	k[5].item = (which == SYMBOLS) ? B_SYMBOLS : B_LETTERS; k[5].n = 6;
}

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

static void repaint(const rv9_mod_env_t *env, const wsk_tree_t *w, wsk_sink_t *s,
                    int y0, int y1)
{
	uint32_t v;
	if (y1 <= y0) return;
	v = ((uint32_t)y0 << 16) | ((uint32_t)y1 & 0xFFFFu);
	env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
	if (wsk_render_rows(w, y0, y1, s, 4096) < 0)
		m_say(env, RV9_STDERR, "wkeys: a repaint was refused\n");
}

static int same(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) { a++; b++; }
	return *a == *b;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	char text[64];
	wsk_buf_t buf[1];
	wsk_tree_t w, k[6];
	int mode = LOWER;
	wsk_ui_t ui;
	wsk_sink_t s;
	out_t out;
	const wsk_tree_t *in = 0;
	uint32_t size = 0;
	int tp, which = -1, e, y0 = 0, y1 = 0;

	if (env == NULL || env->abi_version < 13) return 1;
	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wkeys: stdout is not a window; send it to /w0\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;

	text[0] = '\0';
	buf[0].b = text; buf[0].cap = sizeof text; buf[0].len = 0; buf[0].caret = 0;

	/* ids off: a keyboard is forty-odd widgets, and at about fifteen bytes
	   an id they are what would push the document past SRC_MAX (§2.3). */
	wsk_init(&w, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	w.pad = 0; w.gap = 0; w.font = 28; w.bg = 0x10203au;
	w.item = WIN; w.n = 2;
	w.nest = k; w.nests = 6;
	w.buf = buf; w.bufs = 1;
	wsk_nest_init(&k[0], &w, WSK_ROW);
	k[0].pad = 12; k[0].gap = 12; k[0].item = TOP; k[0].n = 2;
	wsk_nest_init(&k[1], &w, WSK_COL);
	k[1].pad = 6; k[1].gap = 6; k[1].item = KEYS; k[1].n = 4;
	for (int i = 2; i < 6; i++) {
		wsk_nest_init(&k[i], &w, WSK_ROW);
		k[i].pad = 0; k[i].gap = 6;
	}
	layer(k, LOWER);
	k[4].font = 28;
	k[5].font = 22;

	e = wsk_check_in(&w, &which, &in);
	if (e != WSK_OK) {
		m_say(env, RV9_STDERR, "wkeys: the keyboard was refused, ");
		m_num(env, RV9_STDERR, e);
		m_say(env, RV9_STDERR, " at widget ");
		m_num(env, RV9_STDERR, which);
		m_say(env, RV9_STDERR, "\n");
		return 3;
	}

	out.env = env;
	out.path = RV9_STDOUT;
	s.put = to_path; s.ctx = &out; s.len = 0;

	wsk_ui_init(&ui, &w);
	(void)wsk_ui_focus(&ui, "text", &y0, &y1);
	e = wsk_render(&w, &s, 4096);
	if (e < 0) {
		wsk_sink_t count;
		count.put = 0; count.ctx = 0; count.len = 0;
		m_say(env, RV9_STDERR, "wkeys: the screen was refused, ");
		m_num(env, RV9_STDERR, e);
		if (e == WSK_E_BUDGET && wsk_render(&w, &count, 0) > 0) {
			m_say(env, RV9_STDERR, ": it is ");
			m_num(env, RV9_STDERR, count.len);
			m_say(env, RV9_STDERR, " bytes of 4096");
		}
		m_say(env, RV9_STDERR, "\n");
		return 4;
	}
	m_say(env, RV9_STDERR, "wkeys: ");
	m_num(env, RV9_STDERR, e);
	m_say(env, RV9_STDERR, " bytes for the keyboard -- tap keys, Enter prints, Clear empties\n");

	tp = env->open("/touch", RV9_MODE_READ);
	if (tp < 0) return 0;

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
			int got;

			ev.kind = (te[i].kind == RV9_TOUCH_DOWN) ? WSK_EV_DOWN
			        : (te[i].kind == RV9_TOUCH_UP)   ? WSK_EV_UP : WSK_EV_MOVE;
			ev.x = (short)te[i].x;
			ev.y = (short)te[i].y;
			ev.d = 0; ev.key = 0; ev.text[0] = '\0'; ev.ms = te[i].at_ms;
			got = wsk_feed(&ui, &ev, note, 2, &y0, &y1);
			repaint(env, &w, &s, y0, y1);

			for (int j = 0; j < got && j < 2; j++) {
				const wsk_widget_t *key;
				wsk_event_t kv;
				wsk_note_t kn[2];
				int kg;

				if (note[j].kind != WSK_NOTE_CLICK) continue;
				key = &note[j].in->item[note[j].item];
				if (key->kind != WSK_BUTTON) continue;
				if (same(key->text, "123") || same(key->text, "abc") ||
				    same(key->text, "Shift") || same(key->text, "SHIFT")) {
					const char *ids[1] = { "keys" };
					mode = same(key->text, "123") ? SYMBOLS
					     : same(key->text, "Shift") ? UPPER : LOWER;
					layer(k, mode);
					if (wsk_rows(&w, ids, 1, &y0, &y1) == WSK_OK)
						repaint(env, &w, &s, y0, y1);
					continue;
				}
				if (same(key->id, "clear")) {
					text[0] = '\0';
					buf[0].len = 0;
					buf[0].caret = 0;
					{
						const char *ids[1] = { "text" };
						if (wsk_rows(&w, ids, 1, &y0, &y1) == WSK_OK)
							repaint(env, &w, &s, y0, y1);
					}
					continue;
				}

				/* What a keyboard would have sent, fed back to the toolkit. */
				kv.kind = WSK_EV_KEY; kv.key = 0; kv.text[0] = '\0';
				kv.x = 0; kv.y = 0; kv.d = 0; kv.ms = 0;
				if (key->text[1] == '\0') {
					kv.kind = WSK_EV_TEXT;
					kv.text[0] = key->text[0];
					kv.text[1] = '\0';
				} else if (same(key->text, "space")) {
					kv.kind = WSK_EV_TEXT;
					kv.text[0] = ' ';
					kv.text[1] = '\0';
				} else if (same(key->text, "Left"))  kv.key = WSK_KEY_LEFT;
				else if (same(key->text, "Right"))   kv.key = WSK_KEY_RIGHT;
				else if (same(key->text, "Del"))     kv.key = WSK_KEY_BACKSPACE;
				else if (same(key->text, "Enter"))   kv.key = WSK_KEY_ENTER;
				else continue;

				kg = wsk_feed(&ui, &kv, kn, 2, &y0, &y1);
				repaint(env, &w, &s, y0, y1);
				/* Shift is for one letter: after it, lower case again. */
				if (mode == UPPER && kv.kind == WSK_EV_TEXT) {
					const char *ids[1] = { "keys" };
					mode = LOWER;
					layer(k, mode);
					if (wsk_rows(&w, ids, 1, &y0, &y1) == WSK_OK)
						repaint(env, &w, &s, y0, y1);
				}
				if (kg > 0 && kn[0].kind == WSK_NOTE_ENTER) {
					m_say(env, RV9_STDERR, "  entered: ");
					m_say(env, RV9_STDERR, text);
					m_say(env, RV9_STDERR, "\n");
				}
			}
		}
	}
	env->close(tp);
	m_say(env, RV9_STDERR, "  the field held: ");
	m_say(env, RV9_STDERR, text);
	m_say(env, RV9_STDERR, "\n");
	return 0;
}
