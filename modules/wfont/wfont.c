/*
 * wfont -- how small can text get on this panel and still be read?
 *
 *     wfont > /w0
 *
 * Seven rows, the same string at font 24 down to 8, each carrying its own
 * size. Look at the panel from wherever you would really read it; the
 * smallest row you can read is the answer. It draws once and exits.
 *
 * WHY IT IS A PROGRAM AND NOT A PICTURE. whisker needs a floor for "scale the
 * text down until it does not fit" and had been trying to find one off the
 * board. Neither route works:
 *
 *   - The renderer's own signal says where IT struggles, not where a person
 *     stops reading: the ink's peak intensity holds down to font 10 and falls
 *     away at 8, as the 10x20 glyph starts being averaged into its background
 *     faster than it is drawn.
 *   - A PPM on a development monitor cannot answer it either, and that is
 *     arithmetic rather than taste. That monitor is 4.0 px/mm; this panel is
 *     9.7. It has 41 % of the pitch, so a font-10 cell -- 1.03 mm here -- is
 *     4.1 pixels there. It would show a smudge and the floor would come out
 *     two sizes too high. (whisker's finger_size/ got away with exactly this
 *     trick, because a touch target is a SHAPE and loses nothing to a coarser
 *     pitch. Type is the fine detail that pitch carries.)
 *
 * So it has to be drawn on the glass, which is what this is for. A row whose
 * NUMBER you cannot read is below your floor by definition, which makes the
 * card self-scoring.
 *
 * Built from whisker's src/module_font.c; see modules/whisker.from.
 */
/*
 * Labels and nothing else, so the minimal profile -- the same reasoning
 * wstat.c carries. Its stack figure is derived from wstat's, which was
 * bisected against a path-free document; leaving marks compiled in would
 * leave that derivation one edit away from being wrong.
 */
#define WSK_MINIMAL 1

#include "modlib.h"
#include "whisker.h"

#define ROWS 7

static const short SIZE[ROWS] = { 24, 20, 16, 14, 12, 10, 8 };

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

/* "24 Temp 45.3 C" -- the size first, so it is the first thing to go. */
static void label(char *dst, int size)
{
	static const char *T = " Temp 45.3 C";
	int n = 0, k;

	if (size >= 10) dst[n++] = (char)('0' + size / 10);
	dst[n++] = (char)('0' + size % 10);
	for (k = 0; T[k] != '\0' && n < WSK_TEXT_MAX - 1; k++) dst[n++] = T[k];
	dst[n] = '\0';
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t row[ROWS];
	wsk_tree_t t;
	wsk_sink_t s;
	out_t out;
	uint32_t size = 0;
	int i, n;

	if (env == NULL || env->abi_version < 11) return 1;

	/* RV9_GS_SIZE cannot answer "is this a window" -- RBF answers it with a
	   file's length. Probe RV9_SVG_SS_ROWS, which only svgwin answers. */
	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wfont: that is not a window\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;

	for (i = 0; i < ROWS; i++) {
		int k;
		for (k = 0; k < (int)sizeof row[i].id; k++) row[i].id[k] = '\0';
		for (k = 0; k < (int)sizeof row[i].text; k++) row[i].text[k] = '\0';
		row[i].kind = WSK_LABEL;
		row[i].size = SIZE[i];
		row[i].weight = 0;
		row[i].fill = WSK_INHERIT;
		row[i].ink = WSK_INHERIT;
		row[i].mark = 0;
		row[i].id[0] = 'f';
		row[i].id[1] = (char)('0' + i);
		label(row[i].text, SIZE[i]);
	}

	wsk_init(&t, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	t.pad = 2;
	t.gap = 2;
	t.bg = 0x0b1420u;
	t.ink = 0xe8f0f8u;      /* near-white: the best case, so the floor found
	                           here is the kindest one the panel offers */
	t.item = row;
	t.n = ROWS;

	out.env = env;
	out.path = RV9_STDOUT;
	s.put = to_path; s.ctx = &out; s.len = 0;
	n = wsk_render(&t, &s, 4096);
	if (n < 0) {
		m_say(env, RV9_STDERR, "wfont: the card does not fit\n");
		return 3;
	}

	m_say(env, RV9_STDERR, "wfont: ");
	m_num(env, RV9_STDERR, n);
	m_say(env, RV9_STDERR, " bytes. Read it from where you would really read"
	                       " it; the smallest row you can read is the floor.\n");
	return 0;
}
