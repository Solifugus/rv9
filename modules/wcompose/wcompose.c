/*
 * wcompose -- two whisker stacks in one window, and one of them repainted.
 *
 *     wcompose > /w0
 *
 * A toolbar across the top and a pair of buttons under it: two independent
 * widget stacks, one document, one background. Then STOP is marked pressed
 * and only its rows are rewritten through RV9_SVG_SS_ROWS, with BOTH stacks
 * still offered to the document -- so the band comes back complete whichever
 * stack the tapped widget belonged to.
 *
 * WHY IT EXISTS. whisker could not do this until 2026-10-02. Each render
 * painted a backing across the whole device, so a second stack drawn after
 * the first simply erased it, and a keyboard came out as its bottom row.
 * The fix is that the background belongs to the DOCUMENT and never to a
 * stack. This is the program that says whether that is true on the glass.
 *
 * WHAT TO LOOK AT. Three things, in order of how much they would cost to
 * find later:
 *
 *   1  both stacks are on the panel at once, and there is one scene behind
 *      them rather than two;
 *   2  the pressed repaint leaves the stack below it untouched, and the band
 *      comes back carrying the scene rather than the device background --
 *      the same thing `flick` got wrong for months;
 *   3  the corner marks on the pressed button, on the real glass. They are a
 *      quarter of the button's height, capped at 16 px, and they were drawn
 *      at matched physical size on a desktop monitor but never on a 1.9-inch
 *      panel.
 *
 * TWO THINGS FOR THE MODULE RULES, which is half of why the file is shaped
 * this way. whisker's composition API takes a FUNCTION POINTER (the body
 * below, run twice: once to count the bytes, once to write them) and an
 * ARRAY OF TREES. An array of POINTERS to trees would be the natural way to
 * write this and is the construct that cost RV-9 a module rewrite twice, so
 * the API refuses to offer it. There are two bodies rather than one because
 * with one, gcc const-propagates the pointer into a direct call and the
 * double-link check proves nothing about it; choosing between them on the
 * window's own height forces auipc/addi and an indirect call.
 *
 * Nothing is buffered: the sink hands each piece to env->write as it is
 * produced, so a 4 KB document never needs 4 KB of a program's memory.
 *
 * Generated from ~/development/whisker/src/module_compose.c, which the
 * whisker suite links at two bases and compares byte for byte. whisker.h in
 * ../ is a byte-identical copy of whisker's src/whisker.h -- if RV-9 would
 * rather build.conf carried an include directory than keep a copy, say so
 * and whisker will drop the copy.
 */
#include "modlib.h"
#include "whisker.h"

static const wsk_widget_t BAR[] = {
	{ .kind = WSK_BUTTON, .id = "back", .text = "Back", .weight = 1 },
	{ .kind = WSK_BUTTON, .id = "stop", .text = "STOP", .weight = 1,
	  .fill = 0x8f2a2au },
	{ .kind = WSK_BUTTON, .id = "go",   .text = "Go",   .weight = 1 },
};

static const wsk_widget_t BODY[] = {
	{ .kind = WSK_BUTTON, .id = "confirm", .text = "CONFIRM", .weight = 1 },
	{ .kind = WSK_BUTTON, .id = "cancel",  .text = "Cancel",  .weight = 1 },
};

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

/* What wsk_compose runs twice. It reads the context and never writes it,
   because the byte count of the first run is what the second is admitted on. */
typedef struct { wsk_doc_t d; const wsk_tree_t *part; int n; } comp_t;

static int comp_body(wsk_sink_t *s, void *ctx)
{
	comp_t *c = (comp_t *)ctx;
	int i, e;

	wsk_open(&c->d, s);
	for (i = 0; i < c->n; i++) {
		e = wsk_part(&c->d, &c->part[i], s);
		if (e != WSK_OK) return e;
	}
	wsk_shut(s);
	return WSK_OK;
}

/*
 * The same, minus the second stack -- what a panel too short for both would
 * draw, and the reason there are two bodies at all.
 *
 * With one body wsk_compose const-propagates to a direct call and the address
 * is never materialised, so the double-link proves nothing about the one
 * construct this shape exists to check. Checked by disassembling it: `jal
 * comp_body` inside wsk_compose.constprop.0, no auipc in sight. Choosing
 * between two bodies on something only the window knows forces the address
 * into a register, which is the thing that has to be position independent.
 *
 * That is the house rule about controls -- a test satisfied by a harness that
 * ran nothing is not a test -- applied to a build rather than to an assertion.
 */
static int bar_body(wsk_sink_t *s, void *ctx)
{
	comp_t *c = (comp_t *)ctx;

	wsk_open(&c->d, s);
	{
		int e = wsk_part(&c->d, &c->part[0], s);
		if (e != WSK_OK) return e;
	}
	wsk_shut(s);
	return WSK_OK;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t bar[3], keys[2];
	wsk_tree_t part[2];
	wsk_sink_t s;
	comp_t c;
	out_t out;
	const char *ids[1];
	wsk_body_t body;
	uint32_t size = 0;
	int w, h, top;

	if (env == NULL || env->abi_version < 11) return 1;

	/*
	 * RV9_GS_SIZE cannot answer "is this a window" -- it is the generic
	 * how-big-is-this code, and RBF answers it with a file's length. So
	 * probe RV9_SVG_SS_ROWS, which only svgwin answers; 0,0 is the
	 * documented way to say "all of it", so the probe changes nothing.
	 * (wflick had the same hole: `wflick > /r0/x` laid itself out on a 0x0
	 * screen and wrote 127 bytes of nonsense.)
	 */
	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wcompose: that is not a window\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;
	w = (int)(size & 0xFFFFu);
	h = (int)(size >> 16);
	top = (h < 200) ? 60 : 96;           /* a toolbar, not a proportion (§5) */

	wsk_clone(bar, BAR, 3);
	wsk_clone(keys, BODY, 2);

	wsk_init(&part[0], WSK_ROW, w, h);
	part[0].pad = 6; part[0].gap = 6; part[0].bg = 0x10203au;
	wsk_area(&part[0], 0, 0, w, top);
	part[0].item = bar; part[0].n = 3;

	wsk_init(&part[1], WSK_COL, w, h);
	part[1].pad = 6; part[1].gap = 6; part[1].bg = 0x10203au;
	part[1].cross_max = 420;
	wsk_area(&part[1], 0, top, w, h - top);
	part[1].item = keys; part[1].n = 2;

	out.env = env;
	out.path = RV9_STDOUT;

	wsk_doc_init(&c.d, &part[0]);
	c.part = part;
	c.n = 2;

	/* From the window, so the compiler cannot fold it away. See bar_body. */
	body = (h >= top + 80) ? comp_body : bar_body;

	s.put = to_path; s.ctx = &out; s.len = 0;
	if (wsk_compose(body, &c, &s, 4096) < 0) {
		m_say(env, RV9_STDERR, "wcompose: the panel does not fit\n");
		return 3;
	}

	/* And a banded repaint of a widget in ONE stack, with both stacks still
	   offered to the document -- which is the thing §5c is actually for. */
	ids[0] = "stop";
	if (wsk_doc_rows(&c.d, &part[0], ids, 1) != WSK_OK) return 4;
	{
		uint32_t v = ((uint32_t)c.d.y0 << 16) | ((uint32_t)c.d.y1 & 0xFFFFu);
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v) < 0) {
			m_say(env, RV9_STDERR, "wcompose: this window cannot clip rows\n");
			return 5;
		}
	}
	bar[1].mark = WSK_MARK_CORNERS;
	bar[1].ink = 0xffe0e0u;
	s.put = to_path; s.ctx = &out; s.len = 0;
	wsk_compose(body, &c, &s, 4096);

	env->sleep_ms(60);

	{
		uint32_t v = ((uint32_t)c.d.y0 << 16) | ((uint32_t)c.d.y1 & 0xFFFFu);
		env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
	}
	bar[1].mark = 0;
	bar[1].ink = WSK_INHERIT;
	s.put = to_path; s.ctx = &out; s.len = 0;
	wsk_compose(body, &c, &s, 4096);

	return 0;
}
