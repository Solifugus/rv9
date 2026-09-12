/*
 * ed -- a full-screen text editor.
 *
 *   ed /f0/notes      edit a file, making it if it is not there
 *
 *   arrows, home/end, page up/down    move
 *   ^S save   ^X quit   ^K cut line
 *
 * The same binary edits over SSH in a 236-column window and on the panel
 * at thirty by eight, because it asks the path it was given how big it is
 * and never assumes. It re-asks on every redraw, so dragging the corner of
 * a terminal reflows it at the next keystroke -- which is why there is no
 * resize signal here and nothing waiting for one.
 *
 * Two things make this possible and neither is in this file. The console
 * setstats (design section 16) mean cursor and colour work the same on a
 * framebuffer and a terminal. Raw input (RV9_SS_RAW) means an arrow key
 * arrives as the three bytes the terminal actually sent, instead of being
 * filtered out by a line discipline doing its job.
 *
 * The text is one flat buffer with newlines in it. Everything else --
 * which line we are on, where it starts -- is recomputed by scanning.
 * Eight kilobytes is small enough that scanning is free and an index would
 * only be something else to keep correct.
 */
#include "modlib.h"

#define TEXT_MAX  8192
#define PATH_LEN  64
#define OUT_MAX   512
#define KEYBUF    16

/* Keys that are not characters. Above 255 so they cannot collide. */
#define K_UP     256
#define K_DOWN   257
#define K_RIGHT  258
#define K_LEFT   259
#define K_HOME   260
#define K_END    261
#define K_PGUP   262
#define K_PGDN   263
#define K_DEL    264
#define K_ESC    265
#define K_NONE   266

#define CTRL(c) ((c) & 0x1F)

typedef struct {
    char     path[PATH_LEN];
    uint32_t len;              /* bytes of text in use */
    uint32_t cur;              /* cursor, as an offset into text */
    uint32_t top;              /* first line on screen */
    uint32_t left;             /* first column on screen */
    uint32_t goal;             /* column to aim for moving up and down */
    uint32_t rows, cols;
    int      modified;
    int      armed;            /* quit asked once with changes pending */
    int      quit;

    char     msg[48];
    char     out[OUT_MAX];

    uint8_t  keys[KEYBUF];
    uint32_t kn, kp;

    char     text[TEXT_MAX];
} ed_t;

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static uint32_t line_start(ed_t *e, uint32_t at)
{
    while (at > 0 && e->text[at - 1] != '\n') at--;
    return at;
}

static uint32_t line_end(ed_t *e, uint32_t at)
{
    while (at < e->len && e->text[at] != '\n') at++;
    return at;
}

static uint32_t line_of(ed_t *e, uint32_t at)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < at; i++) if (e->text[i] == '\n') n++;
    return n;
}

static uint32_t offset_of_line(ed_t *e, uint32_t line)
{
    uint32_t i = 0, n = 0;
    while (i < e->len && n < line) { if (e->text[i] == '\n') n++; i++; }
    return i;
}

static uint32_t total_lines(ed_t *e)
{
    return line_of(e, e->len) + 1;
}

static void set_msg(ed_t *e, const char *s)
{
    uint32_t i = 0;
    while (s[i] && i < sizeof(e->msg) - 1) { e->msg[i] = s[i]; i++; }
    e->msg[i] = '\0';
}

/* No libc, so the moves are written out. Insert copies backwards and
   delete forwards, which is what keeps an overlapping move correct. */
static void make_room(ed_t *e, uint32_t at, uint32_t n)
{
    for (uint32_t i = e->len; i > at; i--) e->text[i - 1 + n] = e->text[i - 1];
    e->len += n;
}

static void close_up(ed_t *e, uint32_t at, uint32_t n)
{
    for (uint32_t i = at; i + n < e->len; i++) e->text[i] = e->text[i + n];
    e->len -= n;
}

static void insert(ed_t *e, const char *s, uint32_t n)
{
    if (e->len + n > TEXT_MAX) { set_msg(e, "buffer full"); return; }
    make_room(e, e->cur, n);
    for (uint32_t i = 0; i < n; i++) e->text[e->cur + i] = s[i];
    e->cur += n;
    e->modified = 1;
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

static int have(ed_t *e) { return (int)(e->kn - e->kp); }

/*
 * A failed read is the end of the session, not a pause.
 *
 * Over SSH the input goes away when the connection does, and an editor
 * that treated that as "no key yet" would spin on a dead path forever.
 */
static void fill(ed_t *e, const rv9_mod_env_t *env)
{
    e->kn = e->kp = 0;

    int n = env->read(RV9_STDIN, e->keys, KEYBUF);
    if (n > 0) e->kn = (uint32_t)n;
    else       e->quit = 1;
}

/*
 * One key.
 *
 * An escape sequence is only recognised when the whole of it is already in
 * hand: a terminal sends ESC [ A in one go, so anything with ESC as its
 * last byte really was somebody pressing escape. That avoids needing a
 * read with a timeout, which is the usual way this gets solved and would
 * need a mechanism RV-9 does not have.
 */
static int getkey(ed_t *e, const rv9_mod_env_t *env)
{
    if (have(e) == 0) fill(e, env);
    if (have(e) == 0) return K_NONE;

    int c = e->keys[e->kp++];
    if (c != 27) return c;

    if (have(e) < 2) return K_ESC;
    if (e->keys[e->kp] != '[' && e->keys[e->kp] != 'O') return K_ESC;
    e->kp++;

    int b = e->keys[e->kp++];

    switch (b) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    default: break;
    }

    if (b >= '0' && b <= '9') {
        /* "1~" home, "3~" delete, "4~" end, "5~" page up, "6~" page down */
        while (have(e) > 0 && e->keys[e->kp] != '~') e->kp++;
        if (have(e) > 0) e->kp++;

        switch (b) {
        case '1': case '7': return K_HOME;
        case '3':           return K_DEL;
        case '4': case '8': return K_END;
        case '5':           return K_PGUP;
        case '6':           return K_PGDN;
        default:            return K_NONE;
        }
    }

    return K_NONE;
}

/* ------------------------------------------------------------------ */
/* Screen                                                              */
/* ------------------------------------------------------------------ */

static uint32_t u32str(char *out, uint32_t v)
{
    char tmp[12];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static void draw_row(const rv9_mod_env_t *env, int p, ed_t *e, uint32_t row)
{
    m_cursor(env, p, row, 0);
    m_clear(env, p, RV9_CON_CLEAR_EOL);

    uint32_t line = e->top + row;
    if (line >= total_lines(e)) return;

    uint32_t s = offset_of_line(e, line);
    uint32_t t = line_end(e, s);

    if (t - s <= e->left) return;
    s += e->left;

    uint32_t n = t - s;
    if (n > e->cols) n = e->cols;
    if (n > 0) env->write(p, e->text + s, n);
}

static void draw_status(const rv9_mod_env_t *env, int p, ed_t *e)
{
    uint32_t o = 0;
    char *b = e->out;

    b[o++] = ' ';
    for (uint32_t i = 0; e->path[i] && o < OUT_MAX - 64; i++) b[o++] = e->path[i];
    if (e->modified) { b[o++] = ' '; b[o++] = '*'; }

    b[o++] = ' '; b[o++] = ' ';
    o += u32str(b + o, line_of(e, e->cur) + 1);
    b[o++] = ':';
    o += u32str(b + o, e->cur - line_start(e, e->cur) + 1);

    if (e->msg[0]) {
        b[o++] = ' '; b[o++] = ' ';
        for (uint32_t i = 0; e->msg[i] && o < OUT_MAX - 2; i++) b[o++] = e->msg[i];
    }

    while (o < e->cols && o < OUT_MAX) b[o++] = ' ';
    if (o > e->cols) o = e->cols;

    m_cursor(env, p, e->rows - 1, 0);
    m_colour(env, p, RV9_COL_BLACK, RV9_COL_CYAN);
    env->write(p, b, o);
    m_colour(env, p, RV9_COL_DEFAULT, RV9_COL_DEFAULT);
}

/* Keep the cursor on screen, and say whether that moved the view. */
static int scroll_to_fit(ed_t *e)
{
    uint32_t line = line_of(e, e->cur);
    uint32_t col  = e->cur - line_start(e, e->cur);
    uint32_t text_rows = e->rows - 1;
    int moved = 0;

    if (line < e->top) { e->top = line; moved = 1; }
    if (line >= e->top + text_rows) { e->top = line - text_rows + 1; moved = 1; }

    if (col < e->left) { e->left = col; moved = 1; }
    if (col >= e->left + e->cols) { e->left = col - e->cols + 1; moved = 1; }

    return moved;
}

static void place_cursor(const rv9_mod_env_t *env, int p, ed_t *e)
{
    uint32_t line = line_of(e, e->cur);
    uint32_t col  = e->cur - line_start(e, e->cur);
    m_cursor(env, p, line - e->top, col - e->left);
}

static void draw_all(const rv9_mod_env_t *env, int p, ed_t *e)
{
    for (uint32_t r = 0; r + 1 < e->rows; r++) draw_row(env, p, e, r);
    draw_status(env, p, e);
    place_cursor(env, p, e);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static void load(const rv9_mod_env_t *env, ed_t *e)
{
    int p = env->open(e->path, RV9_MODE_READ);
    if (p < 0) { set_msg(e, "new file"); return; }

    for (;;) {
        int n = env->read(p, e->text + e->len, TEXT_MAX - e->len);
        if (n <= 0) break;
        e->len += (uint32_t)n;
        if (e->len >= TEXT_MAX) { set_msg(e, "truncated"); break; }
    }
    env->close(p);
}

static void save(const rv9_mod_env_t *env, ed_t *e)
{
    int p = env->open(e->path, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (p < 0) { set_msg(e, "cannot write"); return; }

    int n = env->write(p, e->text, e->len);
    env->close(p);

    if (n < 0 || (uint32_t)n != e->len) { set_msg(e, "write failed"); return; }

    e->modified = 0;
    set_msg(e, "saved");
}

/* ------------------------------------------------------------------ */
/* Editing                                                             */
/* ------------------------------------------------------------------ */

/* Returns 1 if the whole screen needs redrawing rather than one row. */
static int handle(const rv9_mod_env_t *env, ed_t *e, int k)
{
    uint32_t s, t, col, line;

    switch (k) {
    case K_NONE:
        return 0;

    case K_LEFT:
        if (e->cur > 0) e->cur--;
        e->goal = e->cur - line_start(e, e->cur);
        return 0;

    case K_RIGHT:
        if (e->cur < e->len) e->cur++;
        e->goal = e->cur - line_start(e, e->cur);
        return 0;

    case K_UP:
    case K_DOWN: {
        line = line_of(e, e->cur);
        if (k == K_UP && line == 0) return 0;
        if (k == K_DOWN && line + 1 >= total_lines(e)) return 0;

        s = offset_of_line(e, k == K_UP ? line - 1 : line + 1);
        t = line_end(e, s);
        col = e->goal;
        if (s + col > t) col = t - s;
        e->cur = s + col;
        return 0;
    }

    case K_HOME:
        e->cur = line_start(e, e->cur);
        e->goal = 0;
        return 0;

    case K_END:
        e->cur = line_end(e, e->cur);
        e->goal = e->cur - line_start(e, e->cur);
        return 0;

    case K_PGUP:
    case K_PGDN: {
        uint32_t page = e->rows - 2;
        line = line_of(e, e->cur);
        if (k == K_PGUP) line = (line > page) ? line - page : 0;
        else {
            line += page;
            if (line >= total_lines(e)) line = total_lines(e) - 1;
        }
        s = offset_of_line(e, line);
        t = line_end(e, s);
        col = e->goal;
        if (s + col > t) col = t - s;
        e->cur = s + col;
        return 1;
    }

    case K_DEL:
        if (e->cur < e->len) {
            int nl = (e->text[e->cur] == '\n');
            close_up(e, e->cur, 1);
            e->modified = 1;
            return nl;
        }
        return 0;

    case 8:
    case 127:
        if (e->cur > 0) {
            int nl = (e->text[e->cur - 1] == '\n');
            e->cur--;
            close_up(e, e->cur, 1);
            e->modified = 1;
            e->goal = e->cur - line_start(e, e->cur);
            return nl;
        }
        return 0;

    case 13:
    case 10:
        insert(e, "\n", 1);
        e->goal = 0;
        return 1;

    case 9:
        insert(e, "    ", 4);
        e->goal = e->cur - line_start(e, e->cur);
        return 0;

    case CTRL('S'):
        save(env, e);
        return 0;

    case CTRL('X'):
    case CTRL('Q'):
        /* Leaving with unsaved changes takes two presses, and the second
           has to be the next key -- an arming that survived other typing
           would eventually discard somebody's work. */
        if (e->modified && !e->armed) {
            set_msg(e, "modified -- ^S to save, ^X again to discard");
            e->armed = 1;
            return 0;
        }
        e->quit = 1;
        return 0;

    case CTRL('K'):
        /* Cut to the end of the line, or take the newline if already
           sitting at it -- so repeated ^K removes whole lines. */
        t = line_end(e, e->cur);
        if (e->cur == t && t < e->len) close_up(e, e->cur, 1);
        else if (t > e->cur)           close_up(e, e->cur, t - e->cur);
        else return 0;
        e->modified = 1;
        return 1;

    default:
        if (k >= 32 && k < 127) {
            char c = (char)k;
            insert(e, &c, 1);
            e->goal = e->cur - line_start(e, e->cur);
        }
        return 0;
    }
}

/* ------------------------------------------------------------------ */

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 12) return -1;

    ed_t *e = (ed_t *)env->statics;
    if (e == NULL || env->statics_size < sizeof(*e)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0' || env->arg[0] == ' ') {
        m_say(env, RV9_STDOUT, "usage: ed <file>\n");
        return -3;
    }
    m_word(env->arg, e->path, sizeof(e->path));

    load(env, e);

    int p = RV9_STDOUT;

    /* Keystrokes, not lines. Without this an arrow key never arrives:
       the line discipline drops it before anyone sees it. */
    uint32_t on = 1;
    if (env->setstat(RV9_STDIN, RV9_SS_RAW, &on) < 0) {
        m_say(env, RV9_STDOUT, "ed: this device cannot do raw input\n");
        return -4;
    }

    m_screen(env, p, &e->rows, &e->cols);
    if (e->rows < 2) e->rows = 2;
    m_clear(env, p, RV9_CON_CLEAR_SCREEN);
    scroll_to_fit(e);
    draw_all(env, p, e);

    while (!e->quit) {
        int k = getkey(e, env);
        if (e->quit) break;      /* the path went away under us */

        uint32_t was_rows = e->rows, was_cols = e->cols, was_top = e->top,
                 was_left = e->left;
        uint32_t was_row = line_of(e, e->cur) - e->top;

        e->msg[0] = '\0';
        if (k != CTRL('X') && k != CTRL('Q') && k != K_NONE) e->armed = 0;

        int full = handle(env, e, k);

        /* Ask again every time round: the window may have been resized
           while we were waiting, and asking costs one getstat. */
        m_screen(env, p, &e->rows, &e->cols);
        if (e->rows < 2) e->rows = 2;
        if (e->rows != was_rows || e->cols != was_cols) full = 1;

        if (scroll_to_fit(e)) full = 1;
        if (e->top != was_top || e->left != was_left) full = 1;

        if (full) {
            draw_all(env, p, e);
        } else {
            uint32_t row = line_of(e, e->cur) - e->top;
            draw_row(env, p, e, row);
            if (row != was_row) draw_row(env, p, e, was_row);
            draw_status(env, p, e);
            place_cursor(env, p, e);
        }
    }

    uint32_t off = 0;
    env->setstat(RV9_STDIN, RV9_SS_RAW, &off);

    m_colour(env, p, RV9_COL_DEFAULT, RV9_COL_DEFAULT);
    m_attr(env, p, 0);
    m_clear(env, p, RV9_CON_CLEAR_SCREEN);
    m_cursor(env, p, 0, 0);
    return 0;
}
