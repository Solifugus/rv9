/*
 * wstat -- the board's own numbers, on the panel.
 *
 *     wstat > /w0           until stopped
 *     wstat 120 > /w0       for 120 seconds
 *
 * Die temperature, available heap and uptime, drawn as a panel and kept up
 * to date. It is the third whisker program here, after `wflick` and
 * `wcompose`, and the first that is a program somebody would actually leave
 * running rather than a demonstration of a mechanism.
 *
 * WHY IT LOOKS LIKE THIS. whisker's own session asked Matthew what the C5
 * panel is for, and the answer was that it is write-only with no controls:
 * the console commands, the panel shows. So there is nothing to press here
 * and no focus to move -- a readout is the whole of what that panel is, and
 * this is it.
 *
 * WHAT IT IS CAREFUL ABOUT, and the reason it is worth reading:
 *
 *   - A tick on which no reading changed writes NOTHING. No setstat, no
 *     bytes, no log line. Every band re-parses the whole document, so a
 *     panel that repaints once a second for no reason costs real time on a
 *     board that has other work -- and the cost is invisible on a desk.
 *     The readings are deliberately at a resolution a person would choose
 *     (tenths of a degree, whole minutes) so that most ticks really do have
 *     nothing to say.
 *   - When something does change, only the rows holding it are rewritten,
 *     through RV9_SVG_SS_ROWS, and the band carries the scene's background
 *     rather than the device's -- which is the bug `flick` shipped for
 *     months and the reason whisker computes the row range rather than the
 *     caller guessing it.
 *   - Status goes to STDERR. STDOUT is the window.
 *   - /tsens/0 is optional. If it is not there the row says so rather than
 *     showing a number that stopped being true.
 *
 * Built from whisker's src/module_stat.c; see modules/whisker.from.
 */
/*
 * A readout marks nothing and has no field, so it is built from whisker's
 * minimal profile. Not for the bytes -- 39 KB is the budget and this saves
 * under a kilobyte. It is so that the stack figure below stays true: with
 * marks compiled in but unused, somebody adding WSK_MARK_CORNERS to a row
 * would reach do_path and a 624-byte raster frame against a stack bisected
 * when no path was reachable. Compiled out, that edit is refused by
 * wsk_check with WSK_E_KIND before a byte is written.
 */
#define WSK_MINIMAL 1

#include "modlib.h"
#include "whisker.h"

#define TICK_MS   1000
#define W_TEXT    14          /* every row padded to this, so a fixed-width
                                 face lines the values up under centring */

/* Rows 1..3 are readings; row 0 is the title and never changes. */
enum { R_TITLE = 0, R_TEMP, R_HEAP, R_UP, R_N };

static const wsk_widget_t FACE[] = {
	{ .kind = WSK_LABEL, .id = "title", .text = "RV-9", .size = 28 },
	{ .kind = WSK_LABEL, .id = "temp",  .text = "",     .size = 32 },
	{ .kind = WSK_LABEL, .id = "heap",  .text = "",     .size = 32 },
	{ .kind = WSK_LABEL, .id = "up",    .text = "",     .size = 32 },
};

typedef struct { const rv9_mod_env_t *env; int path; } out_t;

static void to_path(wsk_sink_t *s, const char *b, int n)
{
	out_t *o = (out_t *)s->ctx;
	o->env->write(o->path, b, (unsigned int)n);
}

/* ------------------------------------------------------------------ */
/* Formatting, which a module does for itself: there is no libc here.  */
/* ------------------------------------------------------------------ */

typedef struct { char *b; int n; int cap; } buf_t;

static void b_init(buf_t *o, char *b, int cap) { o->b = b; o->n = 0; o->cap = cap; }

static void b_s(buf_t *o, const char *s)
{
	while (*s != '\0' && o->n < o->cap - 1) o->b[o->n++] = *s++;
}

static void b_u(buf_t *o, uint32_t v)
{
	char t[12];
	int i = 0;
	do { t[i++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u && i < 12);
	while (i > 0 && o->n < o->cap - 1) o->b[o->n++] = t[--i];
}

static void b_end(buf_t *o) { o->b[o->n] = '\0'; }

static int s_len(const char *s) { int n = 0; while (s[n] != '\0') n++; return n; }

/*
 * name + padding + value, to exactly W_TEXT visible characters.
 *
 * The padding goes in the MIDDLE, and that is not a style choice. `/w0`
 * strips whitespace before measuring a <text> (SS2.1), so trailing spaces
 * weigh nothing and a row padded on the right is centred on its visible
 * characters alone -- which leaves every row centred on a different width and
 * the values ragged. Looked at, not reasoned about: the first version of this
 * drew exactly that and the PPM showed it immediately.
 *
 * Padding between the two instead makes every row the same visible width, so
 * the centring the face already does puts the values in a column and the
 * names in another. One fixed-width face, no x arithmetic, no second stack.
 */
static void row(char *dst, const char *name, const char *val)
{
	int n = 0, i, lv = s_len(val);

	for (i = 0; name[i] != '\0' && n < WSK_TEXT_MAX - 1; i++) dst[n++] = name[i];
	while (n < W_TEXT - lv && n < WSK_TEXT_MAX - 1) dst[n++] = ' ';
	for (i = 0; i < lv && n < WSK_TEXT_MAX - 1; i++) dst[n++] = val[i];
	dst[n] = '\0';
}

/* Did this row change? The comparison is the damage model: a row that
   restates what it already said costs no repaint (notation-sketch.md SS3
   reached the same shape from the other end). */
static int differs(const char *a, const char *b)
{
	int i = 0;
	while (a[i] != '\0' && b[i] != '\0') { if (a[i] != b[i]) return 1; i++; }
	return a[i] != b[i];
}

static void copy_text(char *dst, const char *src)
{
	int i = 0;
	while (src[i] != '\0' && i < WSK_TEXT_MAX - 1) { dst[i] = src[i]; i++; }
	dst[i] = '\0';
}

/*
 * To STDERR, always, and that is not fastidiousness: this program is run as
 * `wstat > /w0`, so STDOUT *is* the window. A status line sent there would be
 * appended to the SVG document and dropped by the parser at best.
 */
static void report(const rv9_mod_env_t *env, int bytes, int y0, int y1)
{
	m_say(env, RV9_STDERR, "wstat: ");
	m_num(env, RV9_STDERR, (int32_t)bytes);
	if (y0 < 0) {
		m_say(env, RV9_STDERR, " bytes, the whole panel\n");
		return;
	}
	m_say(env, RV9_STDERR, " bytes, rows ");
	m_num(env, RV9_STDERR, (int32_t)y0);
	m_say(env, RV9_STDERR, "..");
	m_num(env, RV9_STDERR, (int32_t)y1);
	m_say(env, RV9_STDERR, "\n");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
	wsk_widget_t live[R_N];
	wsk_tree_t t;
	wsk_sink_t s;
	out_t out;
	char scratch[R_N][WSK_TEXT_MAX];
	const char *ids[R_N];
	rv9_sys_mem_t mem;
	uint32_t size = 0, deadline = 0, secs = 0;
	int tsens = -1, first = 1, i;

	if (env == NULL || env->abi_version < 11) return 1;
	if (env->time_ms == NULL || env->signals_take == NULL) return 1;

	/* "Is this a window" is not a question RV9_GS_SIZE can answer -- it is
	   the generic how-big-is-this code and RBF answers it with a file's
	   length. Probe RV9_SVG_SS_ROWS, which only svgwin answers, and to
	   which 0,0 means "all of it", so asking changes nothing. */
	{
		uint32_t none = 0;
		if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &none) < 0) {
			m_say(env, RV9_STDERR, "wstat: that is not a window\n");
			return 2;
		}
	}
	if (env->getstat(RV9_STDOUT, RV9_GS_SIZE, &size) < 0) return 2;

	if (env->arg != 0 && env->arg[0] != '\0') {
		secs = m_num_parse(env->arg, 0);
		deadline = (uint32_t)env->time_ms() + secs * 1000u;
	}

	tsens = env->open("/tsens/0", RV9_MODE_READ);   /* may fail; we say so */

	wsk_clone(live, FACE, R_N);
	wsk_init(&t, WSK_COL, (int)(size & 0xFFFFu), (int)(size >> 16));
	t.pad = 6;
	t.gap = 4;
	t.ids = 1;
	t.bg = 0x0b1420u;
	t.ink = 0xd8e4f0u;
	t.font = 24;

	out.env = env;
	out.path = RV9_STDOUT;

	for (;;) {
		int n = 0;

		if (env->signals_take() & RV9_SIG_STOP) break;

		/* --- read, at a resolution a person would choose rather than the
		       finest the source offers: see the note at the top --- */
		{
			char v[WSK_TEXT_MAX];
			buf_t o;

			b_init(&o, v, WSK_TEXT_MAX);
			if (tsens >= 0) {
				uint32_t raw = 0;
				if (env->read(tsens, &raw, sizeof raw) >= 0) {
					int32_t tenths = (int32_t)raw / 10;  /* hundredths C in */
					if (tenths < 0) { b_s(&o, "-"); tenths = -tenths; }
					b_u(&o, (uint32_t)(tenths / 10));
					b_s(&o, ".");
					b_u(&o, (uint32_t)(tenths % 10));
					b_s(&o, " C");
				} else {
					b_s(&o, "read?");
				}
			} else {
				b_s(&o, "none");
			}
			b_end(&o);
			row(scratch[R_TEMP], "TEMP", v);

			b_init(&o, v, WSK_TEXT_MAX);
			if (env->sysinfo(RV9_SYS_MEM, &mem, sizeof mem) >= 1) {
				b_u(&o, mem.heap_available / 1024u);
				b_s(&o, "k");
			} else {
				b_s(&o, "?");
			}
			b_end(&o);
			row(scratch[R_HEAP], "HEAP", v);

			{
				/* Narrowed BEFORE the divide. A module links against no
				   runtime library, so a 64-bit division is an undefined
				   reference rather than an instruction -- uptime.c learned
				   this first, and 32 bits of milliseconds is 49 days. */
				uint32_t mins = (uint32_t)env->time_ms() / 60000u;

				b_init(&o, v, WSK_TEXT_MAX);
				if (mins >= 60u) {
					b_u(&o, mins / 60u); b_s(&o, "h "); mins %= 60u;
				}
				b_u(&o, mins);
				b_s(&o, "m");
				b_end(&o);
				row(scratch[R_UP], "UP", v);
			}
		}

		/* --- which rows actually moved --- */
		for (i = R_TEMP; i < R_N; i++)
			if (differs(live[i].text, scratch[i])) {
				copy_text(live[i].text, scratch[i]);
				ids[n++] = live[i].id;
			}

		if (first) {
			s.put = to_path; s.ctx = &out; s.len = 0;
			if (wsk_render(&t, &s, 4096) < 0) {
				m_say(env, RV9_STDERR, "wstat: the panel does not fit\n");
				return 3;
			}
			report(env, s.len, -1, -1);
			first = 0;
		} else if (n > 0) {
			int y0 = 0, y1 = 0;
			if (wsk_rows(&t, ids, n, &y0, &y1) == WSK_OK) {
				uint32_t v = ((uint32_t)y0 << 16) | ((uint32_t)y1 & 0xFFFFu);
				if (env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v) >= 0) {
					s.put = to_path; s.ctx = &out; s.len = 0;
					wsk_render_clipped(&t, ids, n, &s, 4096);
					report(env, s.len, y0, y1);
				}
			}
		}
		/* and when n == 0, deliberately nothing: no setstat, no write. That
		   silence is the point of the program, so there is nothing to print
		   either -- a line per quiet tick would be the console doing exactly
		   what the panel is being careful not to. */

		if (secs != 0u && (uint32_t)env->time_ms() >= deadline) break;
		env->sleep_ms(TICK_MS);
	}

	if (tsens >= 0) env->close(tsens);
	return 0;
}
