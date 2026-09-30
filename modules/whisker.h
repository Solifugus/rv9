/*
 * whisker -- a widget layer for a window you write SVG to.
 *
 * Iteration 1: labels and buttons in a single-axis stack, column or row.
 * Three calls, all pure, none of which touches a device:
 *
 *     wsk_render(tree, sink, ctx)                     the whole screen
 *     wsk_rows(tree, ids, n, &y0, &y1)                which device rows moved
 *     wsk_render_clipped(tree, ids, n, sink, ctx)     just those rows
 *
 * The host writes what the sink collected to /w0, having first told the
 * window which rows to repaint:
 *
 *     wsk_rows(&t, ids, 1, &y0, &y1);
 *     uint32_t v = ((uint32_t)y0 << 16) | (uint32_t)y1;
 *     env->setstat(w0, RV9_SVG_SS_ROWS, &v);      // one-shot, every time
 *     wsk_render_clipped(&t, ids, 1, sink, &w0);
 *
 * Header-only, like RV-9's modlib.h (D1). No libc, no allocation, no static
 * data, no 64-bit division, no table of pointers -- a program on RV-9 is a
 * module and inherits every rule a module obeys (design.md §4.1).
 *
 * The caller owns the tree (D5) and an id is the address (D4).
 *
 * WHAT A CALLER'S PANEL MAY LOOK LIKE. Widget text sits *inside* the struct
 * rather than behind a `const char *`, so that a panel may be `static const`
 * and live in rodata. A table of pointers is addresses in rodata and does not
 * link into a module; inline arrays do (review-2026-09-30.md §3.1). The tree
 * itself is built at runtime, because it holds a pointer to that array:
 *
 *     static const wsk_widget_t PANEL[] = {
 *         { WSK_LABEL,  "title",  "Settings" },
 *         { WSK_BUTTON, "ok",     "OK" },
 *         { WSK_BUTTON, "cancel", "Cancel" },
 *     };
 *
 *     wsk_tree_t t;
 *     wsk_init(&t, WSK_COL, dev_w, dev_h);     // the size getstat returned
 *     t.item = PANEL;
 *     t.n = 3;
 */
#ifndef WHISKER_H
#define WHISKER_H

/* ------------------------------------------------------------------ */
/* The tree                                                            */
/* ------------------------------------------------------------------ */

#define WSK_ID_MAX    12
#define WSK_TEXT_MAX  32

#define WSK_BAND      8          /* RV-9 rasterises in 8-row bands */
#define WSK_BTN_ROWS  64         /* 8 bands, and about a finger at 170 dpi */

enum { WSK_LABEL = 1, WSK_BUTTON };
enum { WSK_COL = 1, WSK_ROW };

/*
 * A mark on a widget, for the states a flat substrate has no other way to
 * show. No shadow, no gradient, no opacity -- §2.4 -- so a pressed button
 * changes colour, and that is one signal doing all the work. Ticks driven
 * inward from the four corners are a second one, in the same ink as the
 * label, costing four line segments in a single path.
 */
enum { WSK_MARK_CORNERS = 1 };

/*
 * "whatever the tree says", and it is zero on purpose.
 *
 * A panel is written with designated initialisers -- `{ .kind = WSK_BUTTON,
 * .id = "ok", .text = "OK" }` -- so that adding a field to this struct does
 * not break every caller, and so that -Wextra -Werror, which RV-9's module
 * build uses, does not object to the ones left out. Everything omitted is
 * zero, so zero has to mean inherit or the common case would come out black.
 *
 * The cost is that a widget cannot ask for pure black. On this hardware that
 * costs nothing at all: the panel is RGB565, so #010101 and #000000 are the
 * same pixel.
 */
#define WSK_INHERIT   0u

typedef struct {
	unsigned char kind;
	char          id[WSK_ID_MAX];
	char          text[WSK_TEXT_MAX];
	short         size;          /* rows in a column, columns in a row; 0 = default */
	short         weight;        /* share of what is left over; 0 = fixed */
	unsigned int  fill;          /* WSK_INHERIT to take the tree's accent */
	unsigned int  ink;           /* WSK_INHERIT to take the tree's ink */
	unsigned char mark;          /* WSK_MARK_CORNERS, or 0 */
} wsk_widget_t;

typedef struct {
	const wsk_widget_t *item;
	int           n;

	unsigned char axis;          /* WSK_COL or WSK_ROW */

	/*
	 * The device, in pixels, from getstat -- and the viewBox, which is why
	 * it must stay the device. User units are then device rows and wsk_rows
	 * needs no conversion; a document whose viewBox says anything else is
	 * scaled onto the panel and every row number in it becomes a lie. Do
	 * not set these to mean "the part of the screen I am using": that is
	 * what the area below is for.
	 */
	short         w, h;

	/*
	 * Where the stack sits inside it. The whole device after wsk_init, and
	 * wsk_area narrows it -- a toolbar across the top, a column down one
	 * side. Iteration 1 lays out one stack; two stacks are two trees over
	 * one device, each with its own area, which is also how a grid will
	 * arrive without the widget model changing.
	 */
	short         ax, ay, aw, ah;

	short         pad, gap;
	short         font;          /* device pixels; the cell height */

	unsigned int  bg;            /* the scene behind everything */
	unsigned int  ink;           /* text */
	unsigned int  accent;        /* a button */

	unsigned char ids;           /* emit id= attributes: 1 yes, 0 no */
} wsk_tree_t;

typedef struct { short x, y, w, h; } wsk_rect_t;

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

enum {
	WSK_OK          =  0,
	WSK_E_TEXT      = -1,        /* a label the window cannot draw */
	WSK_E_BUDGET    = -2,        /* it will not fit in the bytes allowed */
	WSK_E_NOTFOUND  = -3,        /* no widget by that id */
	WSK_E_EMPTY     = -4         /* an empty row range: nothing to repaint */
};

/* ------------------------------------------------------------------ */
/* The sink                                                            */
/* ------------------------------------------------------------------ */

/*
 * Where the document goes. A sink that appends to an array is the buffer
 * form; a sink that calls env->write is the streaming one. Passing a
 * function pointer is position independent -- only a stored table of them
 * is not.
 *
 * `put` may be null, in which case nothing is written and only the length
 * is counted, which is how the byte budget is checked before anything is
 * emitted rather than after.
 */
typedef struct wsk_sink {
	void (*put)(struct wsk_sink *s, const char *b, int n);
	void  *ctx;
	int    len;                  /* what it would take, written or not */
} wsk_sink_t;

static inline void wsk__w(wsk_sink_t *s, const char *b, int n)
{
	if (s->put != 0) s->put(s, b, n);
	s->len += n;
}

static inline void wsk__s(wsk_sink_t *s, const char *t)
{
	int n = 0;
	while (t[n] != '\0') n++;
	wsk__w(s, t, n);
}

static inline void wsk__n(wsk_sink_t *s, int v)
{
	char b[8];
	int i = 0;
	if (v < 0) { wsk__w(s, "-", 1); v = -v; }
	do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v != 0 && i < 8);
	while (i > 0) { char c = b[--i]; wsk__w(s, &c, 1); }
}

/*
 * Arithmetic rather than a lookup table: a table is a variable at a fixed
 * address, which linked into a module before 2026-09-30 and may still be
 * linked into an older one. RV-9's `dump` learned this and says so.
 */
static inline char wsk__hexdig(unsigned int v)
{
	v &= 0xF;
	return (char)((v < 10) ? ('0' + v) : ('a' + (v - 10)));
}

/*
 * A colour, in the shortest spelling the window accepts. #1a2b3c is six
 * digits; #112233 is three. Three bytes a colour, on every element, on every
 * band of every repaint.
 */
static inline void wsk__col(wsk_sink_t *s, unsigned int rgb)
{
	unsigned int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
	char out[7];
	int n;

	if ((r >> 4) == (r & 0xF) && (g >> 4) == (g & 0xF) && (b >> 4) == (b & 0xF)) {
		out[0] = wsk__hexdig(r);
		out[1] = wsk__hexdig(g);
		out[2] = wsk__hexdig(b);
		n = 3;
	} else {
		out[0] = wsk__hexdig(r >> 4); out[1] = wsk__hexdig(r);
		out[2] = wsk__hexdig(g >> 4); out[3] = wsk__hexdig(g);
		out[4] = wsk__hexdig(b >> 4); out[5] = wsk__hexdig(b);
		n = 6;
	}
	wsk__w(s, "#", 1);
	wsk__w(s, out, n);
}

/* ------------------------------------------------------------------ */
/* Text, which the window has already decided for us                   */
/* ------------------------------------------------------------------ */

/*
 * The font is fixed-width and there is exactly one of it, so every number a
 * layout needs is arithmetic (design.md §2.1). Widths are in device pixels.
 */
#define WSK_GLYPH_W   10
#define WSK_GLYPH_H   20
#define WSK_BASELINE  15

static inline int wsk_advance(int font) { return font * WSK_GLYPH_W / WSK_GLYPH_H; }
static inline int wsk_text_w(int font, int chars) { return chars * wsk_advance(font); }
static inline int wsk_baseline_of(int top, int font)
{
	return top + font * WSK_BASELINE / WSK_GLYPH_H;
}

/*
 * What a label may carry.
 *
 * The window draws entities literally -- `&lt;` is four characters on the
 * glass -- so text cannot be escaped, only kept clean; and a raw '<' hung
 * the driver outright until 2026-09-30. Bytes outside 32..126 advance a cell
 * and draw nothing. So the boundary is here (design.md §2.4).
 */
static inline int wsk_text_ok(const char *t)
{
	for (int i = 0; t[i] != '\0'; i++) {
		char c = t[i];
		if (c < 32 || c > 126) return 0;
		if (c == '<' || c == '&' || c == '>') return 0;
	}
	return 1;
}

/*
 * WSK_OK, or WSK_E_TEXT with `which` naming the widget.
 *
 * The index comes back through a parameter rather than as the return value,
 * because the first widget in a tree has index 0 and so does WSK_OK -- a
 * refusal that cannot be told from an acceptance is worse than no check.
 * `which` may be null when only the verdict is wanted.
 */
static inline int wsk_check(const wsk_tree_t *t, int *which)
{
	for (int i = 0; i < t->n; i++)
		if (!wsk_text_ok(t->item[i].text)) {
			if (which != 0) *which = i;
			return WSK_E_TEXT;
		}
	return WSK_OK;
}

static inline int wsk__len(const char *s)
{
	int n = 0;
	while (s[n] != '\0') n++;
	return n;
}

/*
 * Copy a panel out of rodata so it can be changed.
 *
 * A caller keeps its panel `static const` -- that is the point of the text
 * being inline -- and then wants a mutable copy to press a button in. The
 * obvious `live[i] = PANEL[i]` compiles to a call to memcpy, which a module
 * has no way to link: the build fails, which is the good case. So the copy
 * is spelled out, and the strings are copied by a loop that stops at a NUL,
 * which cannot be turned back into a memcpy because its length is not known
 * until it runs.
 */
static inline void wsk__scpy(char *d, const char *s, int cap)
{
	int i = 0;
	while (s[i] != '\0' && i < cap - 1) { d[i] = s[i]; i++; }
	d[i] = '\0';
}

static inline void wsk_clone(wsk_widget_t *dst, const wsk_widget_t *src, int n)
{
	for (int i = 0; i < n; i++) {
		dst[i].kind   = src[i].kind;
		dst[i].size   = src[i].size;
		dst[i].weight = src[i].weight;
		dst[i].fill   = src[i].fill;
		dst[i].ink    = src[i].ink;
		dst[i].mark   = src[i].mark;
		wsk__scpy(dst[i].id, src[i].id, WSK_ID_MAX);
		wsk__scpy(dst[i].text, src[i].text, WSK_TEXT_MAX);
	}
}

static inline int wsk__same(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) { a++; b++; }
	return *a == *b;
}

/* ------------------------------------------------------------------ */
/* Layout: one axis, and nothing else                                  */
/* ------------------------------------------------------------------ */

static inline void wsk_init(wsk_tree_t *t, int axis, int dev_w, int dev_h)
{
	t->item = 0;
	t->n = 0;
	t->axis = (unsigned char)axis;
	t->w = (short)dev_w;
	t->h = (short)dev_h;
	t->ax = 0;
	t->ay = 0;
	t->aw = (short)dev_w;
	t->ah = (short)dev_h;
	t->pad = 8;
	t->gap = 8;
	t->font = 16;
	t->bg = 0x102030u;
	t->ink = 0xffffffu;
	t->accent = 0x2a6b8fu;
	t->ids = 0;
}

/* The part of the device this stack lays out in. */
static inline void wsk_area(wsk_tree_t *t, int x, int y, int w, int h)
{
	t->ax = (short)x;
	t->ay = (short)y;
	t->aw = (short)w;
	t->ah = (short)h;
}

static inline int wsk__round_band(int v)
{
	return ((v + WSK_BAND - 1) / WSK_BAND) * WSK_BAND;
}

/*
 * A widget's natural extent along the stack's axis, before weights.
 *
 * Rounded up to a whole band where the caller did not say otherwise: the
 * clip snaps outward anyway, so a widget that straddles a boundary costs an
 * extra band for nothing (design.md §5, criterion 4).
 */
static inline int wsk__natural(const wsk_tree_t *t, int i)
{
	const wsk_widget_t *w = &t->item[i];
	if (w->size > 0) return w->size;
	if (t->axis == WSK_ROW) {
		int text = wsk_text_w(t->font, wsk__len(w->text));
		return text + 2 * t->pad;
	}
	if (w->kind == WSK_BUTTON) return WSK_BTN_ROWS;
	return wsk__round_band(t->font + t->gap);
}

/*
 * Where widget i sits, in device pixels.
 *
 * Fixed extents first, then whatever is left over is shared by weight. Sizes
 * are pixels and not proportions, which is what makes the layer scale: a
 * repaint costs (its bands / total bands) of a frame, so a fixed-height
 * widget costs the same in absolute terms on a large screen as on a small
 * one. A bigger screen holds more widgets rather than bigger ones.
 */
static inline wsk_rect_t wsk_rect(const wsk_tree_t *t, int idx)
{
	wsk_rect_t r;
	int span = (t->axis == WSK_COL ? t->ah : t->aw) - 2 * t->pad;
	int used = 0, weights = 0, at, extra, given = 0;

	for (int i = 0; i < t->n; i++) {
		used += wsk__natural(t, i);
		weights += t->item[i].weight;
	}
	if (t->n > 1) used += (t->n - 1) * t->gap;
	extra = span - used;
	if (extra < 0) extra = 0;

	at = (t->axis == WSK_COL ? t->ay : t->ax) + t->pad;
	for (int i = 0; i < idx; i++) {
		int e = wsk__natural(t, i);
		if (weights > 0 && t->item[i].weight > 0) {
			int share = extra * t->item[i].weight / weights;
			e += share;
			given += share;
		}
		at += e + t->gap;
	}

	{
		int e = wsk__natural(t, idx);
		if (weights > 0 && t->item[idx].weight > 0) {
			/* the last weighted child takes the rounding, so the stack
			   ends where the padding says it should */
			int last = -1;
			for (int i = 0; i < t->n; i++) if (t->item[i].weight > 0) last = i;
			e += (idx == last) ? (extra - given)
			                   : (extra * t->item[idx].weight / weights);
		}
		if (t->axis == WSK_COL) {
			r.x = (short)(t->ax + t->pad);
			r.w = (short)(t->aw - 2 * t->pad);
			r.y = (short)at;
			r.h = (short)e;
		} else {
			r.y = (short)(t->ay + t->pad);
			r.h = (short)(t->ah - 2 * t->pad);
			r.x = (short)at;
			r.w = (short)e;
		}
	}
	return r;
}

static inline int wsk_find(const wsk_tree_t *t, const char *id)
{
	for (int i = 0; i < t->n; i++)
		if (wsk__same(t->item[i].id, id)) return i;
	return WSK_E_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/* Rows: what a repaint costs, in the units the setstat wants          */
/* ------------------------------------------------------------------ */

/*
 * The device rows these widgets occupy, snapped outward to whole bands.
 *
 * Snapped, and that is not a detail. The driver snaps the clip outward and
 * clears every band it visits in full, so a document whose backing covers
 * only the widgets' own rows leaves a strip of the device colour at each end
 * -- which is the bug `flick` shipped for months. These are the rows to hand
 * RV9_SVG_SS_ROWS *and* the rows the backing must cover; one number, so the
 * two cannot disagree.
 *
 * A set rather than one id, because a real interface changes more than one
 * widget at a time and the single form cannot be retrofitted once callers
 * have written loops (rv9-answers.md §3.3).
 */
static inline int wsk_rows(const wsk_tree_t *t, const char *const *ids, int n,
                           int *y0, int *y1)
{
	int lo = t->h, hi = 0;

	for (int k = 0; k < n; k++) {
		int i = wsk_find(t, ids[k]);
		wsk_rect_t r;
		if (i < 0) return WSK_E_NOTFOUND;
		r = wsk_rect(t, i);
		if (r.y < lo) lo = r.y;
		if (r.y + r.h > hi) hi = r.y + r.h;
	}
	if (hi <= lo) return WSK_E_EMPTY;

	lo = (lo / WSK_BAND) * WSK_BAND;
	hi = wsk__round_band(hi);
	if (lo < 0) lo = 0;
	if (hi > t->h) hi = t->h;

	*y0 = lo;
	*y1 = hi;
	return WSK_OK;
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

static inline void wsk__rect(wsk_sink_t *s, const wsk_tree_t *t,
                             const wsk_widget_t *w, wsk_rect_t r)
{
	wsk__s(s, "<rect");
	if (w != 0 && t->ids) { wsk__s(s, " id=\""); wsk__s(s, w->id); wsk__s(s, "\""); }
	if (r.x != 0) { wsk__s(s, " x=\""); wsk__n(s, r.x); wsk__s(s, "\""); }
	if (r.y != 0) { wsk__s(s, " y=\""); wsk__n(s, r.y); wsk__s(s, "\""); }
	wsk__s(s, " width=\""); wsk__n(s, r.w);
	wsk__s(s, "\" height=\""); wsk__n(s, r.h);
	wsk__s(s, "\" fill=\"");
	wsk__col(s, (w != 0 && w->fill != WSK_INHERIT) ? w->fill
	          : (w != 0 ? t->accent : t->bg));
	wsk__s(s, "\"/>");
}

/*
 * Ticks driven inward from the four corners.
 *
 * One path, four subpaths, and no `fill="none"`: a two-point contour has no
 * area, so the <g>'s fill draws nothing and the attribute would be twelve
 * bytes to say so. The arm is a quarter of the widget's height, capped, so
 * the mark keeps its proportions on a seven-inch panel and its presence on a
 * small one; below three pixels it is not drawn, because a mark nobody can
 * see is bytes re-parsed once per band for nothing.
 */
static inline void wsk__corner(wsk_sink_t *s, int x, int y, int dx, int dy)
{
	wsk__s(s, "M");
	wsk__n(s, x);
	wsk__s(s, " ");
	wsk__n(s, y);
	wsk__s(s, "l");
	wsk__n(s, dx);
	wsk__s(s, " ");
	wsk__n(s, dy);
}

static inline void wsk__marks(wsk_sink_t *s, const wsk_tree_t *t,
                              const wsk_widget_t *w, wsk_rect_t r)
{
	int a = r.h / 4;

	if (a > 16) a = 16;
	if (a < 3) return;

	wsk__s(s, "<path d=\"");
	wsk__corner(s, r.x,        r.y,        a,  a);
	wsk__corner(s, r.x + r.w,  r.y,       -a,  a);
	wsk__corner(s, r.x,        r.y + r.h,  a, -a);
	wsk__corner(s, r.x + r.w,  r.y + r.h, -a, -a);
	wsk__s(s, "\" stroke=\"");
	wsk__col(s, (w->ink != WSK_INHERIT) ? w->ink : t->ink);
	wsk__s(s, "\" stroke-width=\"2\"/>");
}

/*
 * A label, centred in its widget, truncated to what fits.
 *
 * Truncated because there is no clipPath in the subset, so an overlong label
 * runs out of its button and over its neighbour rather than being cut off.
 * text-anchor does the centring, against the same advance box §2.1 gives --
 * one attribute instead of a computed x, and fewer bytes.
 */
static inline void wsk__text(wsk_sink_t *s, const wsk_tree_t *t,
                             const wsk_widget_t *w, wsk_rect_t r)
{
	int chars = wsk__len(w->text);
	int room = (r.w - 2) / wsk_advance(t->font);
	int top = r.y + (r.h - t->font) / 2;

	if (room < 1) return;
	if (chars > room) chars = room;
	if (chars == 0) return;

	wsk__s(s, "<text x=\"");
	wsk__n(s, r.x + r.w / 2);
	wsk__s(s, "\" y=\"");
	wsk__n(s, wsk_baseline_of(top, t->font));
	/* Only when it differs from the <g>, so the common case still costs
	   nothing. A pressed button needs it: white on a pale fill is legible
	   in a test and not on the glass, which is what the picture showed. */
	if (w->ink != WSK_INHERIT) {
		wsk__s(s, "\" fill=\"");
		wsk__col(s, w->ink);
	}
	wsk__s(s, "\">");
	wsk__w(s, w->text, chars);
	wsk__s(s, "</text>");
}

/*
 * The document.
 *
 * `y0`/`y1` bound what is drawn: the whole screen when they are 0 and t->h,
 * a clipped repaint otherwise. Either way it is a COMPLETE document with the
 * same viewBox -- there is no such thing as a fragment here -- and it
 * carries the backing for its own rows, because a band is cleared to the
 * device colour before anything is drawn into it.
 *
 * The viewBox is the device, deliberately: user units are then device rows
 * and wsk_rows needs no conversion. A whisker that assumed that instead of
 * arranging it works at 320x172 and clips the wrong strip at 1024x600.
 */
static inline void wsk__doc(const wsk_tree_t *t, int y0, int y1, wsk_sink_t *s)
{
	wsk_rect_t back;

	wsk__s(s, "<svg viewBox=\"0 0 ");
	wsk__n(s, t->w);
	wsk__s(s, " ");
	wsk__n(s, t->h);
	wsk__s(s, "\">");

	back.x = 0;
	back.y = (short)y0;
	back.w = t->w;
	back.h = (short)(y1 - y0);
	wsk__rect(s, t, 0, back);

	/* font-size, anchor and ink on one <g>: measured at 44 % more interface
	   under SRC_MAX than spelling them on every element, and re-parsed once
	   per band either way. */
	wsk__s(s, "<g font-size=\"");
	wsk__n(s, t->font);
	wsk__s(s, "\" text-anchor=\"middle\" fill=\"");
	wsk__col(s, t->ink);
	wsk__s(s, "\">");

	for (int i = 0; i < t->n; i++) {
		const wsk_widget_t *w = &t->item[i];
		wsk_rect_t r = wsk_rect(t, i);
		if (r.y >= y1 || r.y + r.h <= y0) continue;      /* not in these rows */
		if (w->kind == WSK_BUTTON) wsk__rect(s, t, w, r);
		if (w->mark == WSK_MARK_CORNERS) wsk__marks(s, t, w, r);
		wsk__text(s, t, w, r);
	}

	wsk__s(s, "</g></svg>");
}

/*
 * Measure, then emit -- so a document that will not fit is refused before a
 * byte of it is written rather than after.
 *
 * That is not tidiness. Past SRC_MAX the window drops the document, and on
 * an RV-9 older than 2026-09-30 it stopped answering altogether. A caller
 * that adds a widget at runtime has to be told, and a half-written picture
 * is the one answer that helps nobody (design.md §2.3).
 */
static inline int wsk__emit(const wsk_tree_t *t, int y0, int y1,
                            wsk_sink_t *sink, int budget)
{
	wsk_sink_t dry;

	if (wsk_check(t, 0) != WSK_OK) return WSK_E_TEXT;

	dry.put = 0;
	dry.ctx = 0;
	dry.len = 0;
	wsk__doc(t, y0, y1, &dry);
	if (budget > 0 && dry.len > budget) return WSK_E_BUDGET;

	wsk__doc(t, y0, y1, sink);
	return dry.len;
}

/* The whole screen. Returns the bytes written, or a negative WSK_E_*. */
static inline int wsk_render(const wsk_tree_t *t, wsk_sink_t *sink, int budget)
{
	return wsk__emit(t, 0, t->h, sink, budget);
}

/*
 * Just the rows those widgets occupy, backing and all.
 *
 * Pair it with wsk_rows and give that same range to RV9_SVG_SS_ROWS. The
 * two agree by construction: both snap outward to the band, so what the
 * driver clears is exactly what this document paints.
 */
static inline int wsk_render_clipped(const wsk_tree_t *t, const char *const *ids,
                                     int n, wsk_sink_t *sink, int budget)
{
	int y0, y1, e = wsk_rows(t, ids, n, &y0, &y1);
	if (e != WSK_OK) return e;
	return wsk__emit(t, y0, y1, sink, budget);
}

#endif /* WHISKER_H */
