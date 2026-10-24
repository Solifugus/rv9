/*
 * wslop -- how far from where you meant does a tap land?
 *
 *     wslop > /w0          five marks, eight taps each
 *     wslop 12 > /w0       twelve taps each
 *
 * Tap the CENTRE of each mark. It moves to the next when it has enough, and
 * prints what it found to stderr, so run it with the console in view.
 *
 * WHY. whisker's design left touch *slop* open on purpose and said what would
 * close it: a real capacitive surface, because a mouse reports the pixel you
 * were over and has no scatter to measure. The P4 has one now, and `wtap`
 * showed the loop closes; this measures how forgiving that loop has to be.
 *
 * It separates three things that one number would hide:
 *
 *   BIAS      the mean offset from the mark. A tap that lands consistently
 *             five pixels high is calibration or parallax -- the driver's or
 *             the glass's, and no tolerance in whisker should paper over it.
 *   SCATTER   the spread about that mean. This is whisker's question: the
 *             radius a hit test must forgive, and the distance two widgets
 *             must exceed or a confident tap lands on the neighbour.
 *   DRIFT     how far the finger travels between DOWN and UP. If that crosses
 *             a widget then press and release belong to different ones, and
 *             the caller needs a policy -- whisker has none, holding no state.
 *
 * Five marks rather than one because scatter need not be uniform: the corners
 * are where a capacitive grid has least to interpolate from, and a figure
 * taken only in the middle would be the flattering one.
 *
 * Pixels AND millimetres, via RV9_GS_PHYSICAL -- which is what makes a
 * tolerance portable between two panels of different pitch instead of being
 * true of one of them. A panel that answers UNKNOWN or UNSUPPORTED just gets
 * the pixel figures, which is the half whisker actually needs.
 *
 * Built from whisker's src/module_slop.c; see modules/whisker.from.
 */
/*
 * Marks and the field compiled out (D8). This program draws a label and a
 * filled box and nothing else, so WSK_MINIMAL costs it nothing -- and it buys
 * something worth more than the 668 bytes: corner marks are the only <path>
 * whisker can emit, and a document with no path never reaches RV-9's do_path
 * or rv9_raster_fill_n, which is a 624-byte frame on the writer's own stack.
 * Compiled out, "this document happens to contain no path" becomes a property
 * of the binary, so the stack figure below stays true if somebody edits this.
 *
 * Only safe since 2026-10-04: modules/whisker.h predated profiles until then,
 * and this define would have compiled cleanly, changed nothing, and left the
 * path in -- a false guarantee readable in the source. whisker.from now says
 * e7d1a4a, which has them.
 */
#define WSK_MINIMAL 1

#include "modlib.h"
#include "whisker.h"

#define TARGETS   5
#define MAX_TAPS  32
#define MARK      14          /* the mark's box, device pixels */
#define STRAY     100         /* further than this, it was not aimed here */
#define SETTLE    400         /* ms after a mark moves, a tap was meant for the last one */
#define EDGE      4           /* the edge bars' thickness, device pixels */

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

/* Integer square root: a module has no libm, and the radius wants to be a
   radius rather than a sum of squares nobody can picture. */
static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0, b = 1u << 30;

	while (b > v) b >>= 2;
	while (b != 0) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return r;
}

static void say_num(const rv9_mod_env_t *env, const char *before, int32_t v,
                    const char *after)
{
	if (before != 0) m_say(env, RV9_STDERR, before);
	m_num(env, RV9_STDERR, v);
	if (after != 0) m_say(env, RV9_STDERR, after);
}

/*
 * Micrometres for a count of pixels, or -1 when the panel will not say how
 * big it is.
 *
 * 32-bit throughout, and that is a rule rather than a preference: the first
 * version of this reached for an int64_t and the linker answered with an
 * undefined reference to __divdi3, because a module links against no runtime
 * library. The product is safe unsigned -- a 1024-pixel panel 153,000 um wide
 * is 157 million, well inside 32 bits -- and the sign is handled outside it.
 */
static int32_t to_um(const rv9_physical_t *p, int px)
{
	uint32_t mag;

	if (p->kind == RV9_PHYS_UNKNOWN || p->width_px == 0 || p->width_um == 0)
		return -1;
	mag = (uint32_t)(px < 0 ? -px : px);
	mag = mag * p->width_um / p->width_px;
	return (px < 0) ? -(int32_t)mag : (int32_t)mag;
}

static void say_px_mm(const rv9_mod_env_t *env, const rv9_physical_t *p,
                      const char *label, int px)
{
	int32_t um = to_um(p, px);

	m_say(env, RV9_STDERR, label);
	m_num(env, RV9_STDERR, px);
	m_say(env, RV9_STDERR, " px");
	if (um >= 0) {
		m_say(env, RV9_STDERR, " (");
		m_num(env, RV9_STDERR, um / 1000);
		m_say(env, RV9_STDERR, ".");
		m_num(env, RV9_STDERR, (um % 1000) / 100);
		m_say(env, RV9_STDERR, " mm)");
	}
	m_say(env, RV9_STDERR, "\n");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t mark[1], head[1], bar[2];
	wsk_tree_t part[4];
	wsk_doc_t doc;
	wsk_sink_t s;
	out_t out;
	rv9_physical_t phys;
	int tx[TARGETS], ty[TARGETS];
	int dx[MAX_TAPS], dy[MAX_TAPS], drift[MAX_TAPS];
	uint32_t size = 0;
	int w, h, taps = 8, t, i, n, touch;
	int down_x = 0, down_y = 0, have_down = 0, fresh = 0;
	uint32_t last_up = 0;

	if (env == NULL || env->abi_version < 11) return 1;

	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wslop: that is not a window\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;
	w = (int)(size & 0xFFFFu);
	h = (int)(size >> 16);

	/* Ask how big the panel is in the world. A device with no opinion says
	   UNSUPPORTED and a device that knows the question but not the answer
	   says UNKNOWN; both mean "work in pixels" and both are fine here --
	   the pixel figures are the ones whisker needs, the millimetres are
	   what make them mean the same thing on another panel. */
	phys.kind = RV9_PHYS_UNKNOWN;
	phys.width_px = 0; phys.width_um = 0;
	if (env->getstat(RV9_STDOUT, RV9_GS_PHYSICAL, &phys) < 0)
		phys.kind = RV9_PHYS_UNKNOWN;

	if (env->arg != 0 && env->arg[0] != '\0') {
		uint32_t v = m_num_parse(env->arg, 0);
		if (v >= 3 && v <= MAX_TAPS) taps = (int)v;
	}

	touch = env->open("/touch", RV9_MODE_READ);
	if (touch < 0) {
		m_say(env, RV9_STDERR, "wslop: /touch will not open\n");
		return 3;
	}

	/* Centre, then the four corners inset by a mark's width: the corners
	   are where a capacitive grid interpolates with least to work with. */
	tx[0] = w / 2;          ty[0] = h / 2;
	tx[1] = MARK * 2;       ty[1] = MARK * 2;
	tx[2] = w - MARK * 2;   ty[2] = MARK * 2;
	tx[3] = MARK * 2;       ty[3] = h - MARK * 2;
	tx[4] = w - MARK * 2;   ty[4] = h - MARK * 2;

	out.env = env;
	out.path = RV9_STDOUT;

	m_say(env, RV9_STDERR, "wslop: tap the CENTRE of each mark, ");
	m_num(env, RV9_STDERR, taps);
	m_say(env, RV9_STDERR, " times. Aim; do not hurry.\n");

	for (t = 0; t < TARGETS; t++) {
		int sx = 0, sy = 0, mx, my, worst = 0, worst_drift = 0, rms = 0;

		n = 0;
		fresh = (t > 0);
		while (n < taps) {
			/* Draw the mark where this target is, and a count above it. */
			{
				int k;
				for (k = 0; k < (int)sizeof mark[0].id; k++) mark[0].id[k] = '\0';
				for (k = 0; k < (int)sizeof mark[0].text; k++) mark[0].text[k] = '\0';
				for (k = 0; k < (int)sizeof head[0].id; k++) head[0].id[k] = '\0';
				for (k = 0; k < (int)sizeof head[0].text; k++) head[0].text[k] = '\0';
			}
			mark[0].kind = WSK_BUTTON;
			mark[0].size = 0; mark[0].weight = 1;
			mark[0].fill = 0xf0d070u; mark[0].ink = WSK_INHERIT; mark[0].mark = 0;
			mark[0].id[0] = 'm';

			head[0].kind = WSK_LABEL;
			head[0].size = 0; head[0].weight = 1;
			head[0].fill = WSK_INHERIT; head[0].ink = WSK_INHERIT; head[0].mark = 0;
			head[0].id[0] = 'h';
			head[0].text[0] = (char)('1' + t);
			head[0].text[1] = '/';
			head[0].text[2] = (char)('0' + TARGETS);
			head[0].text[3] = ' ';
			head[0].text[4] = (char)('0' + n / 10);
			head[0].text[5] = (char)('0' + n % 10);
			head[0].text[6] = '/';
			head[0].text[7] = (char)('0' + taps / 10);
			head[0].text[8] = (char)('0' + taps % 10);
			head[0].text[9] = '\0';

			wsk_init(&part[0], WSK_COL, w, h);
			part[0].pad = 1; part[0].gap = 0; part[0].bg = 0x0b1420u;
			wsk_area(&part[0], tx[t] - MARK / 2, ty[t] - MARK / 2, MARK, MARK);
			part[0].item = mark; part[0].n = 1;

			wsk_init(&part[1], WSK_COL, w, h);
			part[1].pad = 2; part[1].gap = 0; part[1].font = 20;
			part[1].bg = 0x0b1420u; part[1].ink = 0x90a8c0u;
			wsk_area(&part[1], 0, 0, w, 28);
			part[1].item = head; part[1].n = 1;

			/* A bar along the top and bottom rows of the panel, red above
			   and cyan below, so a band of colour at an edge says which
			   rows it is. If either is missing or thick, the picture is
			   not where the window says it is, and every bias below is
			   the display's, not the touch's -- which the first run on the
			   P4 could not tell apart, because nobody could see the bottom
			   marks whole. */
			for (i = 0; i < 2; i++) {
				int k;
				for (k = 0; k < (int)sizeof bar[i].id; k++) bar[i].id[k] = '\0';
				for (k = 0; k < (int)sizeof bar[i].text; k++) bar[i].text[k] = '\0';
				bar[i].kind = WSK_BUTTON;
				bar[i].size = 0; bar[i].weight = 1;
				bar[i].ink = WSK_INHERIT; bar[i].mark = 0;
				bar[i].id[0] = 'e';
				bar[i].id[1] = (char)('0' + i);
				wsk_init(&part[2 + i], WSK_COL, w, h);
				part[2 + i].pad = 0; part[2 + i].gap = 0;
				part[2 + i].item = &bar[i]; part[2 + i].n = 1;
			}
			bar[0].fill = 0xe04040u;
			bar[1].fill = 0x40e0e0u;
			wsk_area(&part[2], 0, 0, w, EDGE);
			wsk_area(&part[3], 0, h - EDGE, w, EDGE);

			wsk_doc_init(&doc, &part[0]);
			{
				uint32_t v = 0;
				env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
			}
			s.put = to_path; s.ctx = &out; s.len = 0;
			wsk_open(&doc, &s);
			wsk_part(&doc, &part[0], &s);
			wsk_part(&doc, &part[1], &s);
			wsk_part(&doc, &part[2], &s);
			wsk_part(&doc, &part[3], &s);
			wsk_shut(&s);

			/* Collect one tap: a DOWN, then the UP that ends it. */
			for (;;) {
				rv9_touch_event_t ev;

				if (env->signals_take() & RV9_SIG_STOP) goto done;
				if (env->read(touch, &ev, sizeof ev) < (int)sizeof ev) continue;

				if (ev.kind == RV9_TOUCH_DOWN) {
					int ox = (int)ev.x - tx[t], oy = (int)ev.y - ty[t];

					/* Two ways a tap is not about this mark, each said
					   aloud so a rejection is never silent: it came while
					   the person was still aiming at the last one, or it
					   landed nowhere near this one. Neither may enter the
					   scatter, which is the spread of taps that were aimed. */
					if (fresh && ev.at_ms - last_up < SETTLE) {
						say_num(env, "  ignored ", (int32_t)ev.x, ",");
						say_num(env, 0, (int32_t)ev.y, " -- too soon after the mark moved\n");
						continue;
					}
					if (ox < -STRAY || ox > STRAY || oy < -STRAY || oy > STRAY) {
						say_num(env, "  ignored ", (int32_t)ev.x, ",");
						say_num(env, 0, (int32_t)ev.y, " -- not aimed at this mark\n");
						continue;
					}
					down_x = (int)ev.x; down_y = (int)ev.y; have_down = 1;
				} else if (ev.kind == RV9_TOUCH_UP && have_down) {
					int ddx = (int)ev.x - down_x, ddy = (int)ev.y - down_y;
					int d;

					if (ddx < 0) ddx = -ddx;
					if (ddy < 0) ddy = -ddy;
					d = (int)isqrt((uint32_t)(ddx * ddx + ddy * ddy));

					dx[n] = down_x - tx[t];
					dy[n] = down_y - ty[t];
					drift[n] = d;
					sx += dx[n]; sy += dy[n];
					have_down = 0;
					fresh = 0;
					last_up = ev.at_ms;
					say_num(env, "  tap ", n + 1, "  down ");
					say_num(env, 0, down_x, ",");
					say_num(env, 0, down_y, "  up ");
					say_num(env, 0, (int32_t)ev.x, ",");
					say_num(env, 0, (int32_t)ev.y, "\n");
					n++;
					break;
				}
			}
		}

		/* Bias is the mean; scatter is the spread about it. */
		mx = sx / n; my = sy / n;
		for (i = 0; i < n; i++) {
			int ex = dx[i] - mx, ey = dy[i] - my;
			int d = (int)isqrt((uint32_t)(ex * ex + ey * ey));
			if (d > worst) worst = d;
			rms += d * d;
			if (drift[i] > worst_drift) worst_drift = drift[i];
		}
		rms = (int)isqrt((uint32_t)(rms / n));

		say_num(env, "\nmark ", t + 1, " at ");
		say_num(env, 0, tx[t], ",");
		say_num(env, 0, ty[t], "\n");
		m_say(env, RV9_STDERR, "  bias    ");
		m_num(env, RV9_STDERR, mx);
		m_say(env, RV9_STDERR, ",");
		m_num(env, RV9_STDERR, my);
		m_say(env, RV9_STDERR, " px -- a constant offset is the driver's, not whisker's\n");
		say_px_mm(env, &phys, "  scatter rms   ", rms);
		say_px_mm(env, &phys, "  scatter worst ", worst);
		say_px_mm(env, &phys, "  drift down-up ", worst_drift);
	}

done:
	env->close(touch);
	m_say(env, RV9_STDERR,
	      "\nthe worst SCATTER is the radius a hit test must forgive, and two\n"
	      "widgets must be further apart than that or a confident tap lands on\n"
	      "the neighbour. BIAS is a different fault and belongs further down.\n");
	return 0;
}
