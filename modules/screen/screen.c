/*
 * screen -- prove that a program can address a screen it knows nothing about.
 *
 *     screen           draw on wherever stdout goes
 *     screen /term     draw on the LCD panel
 *
 * The same code produces the same picture on the panel and in a terminal
 * over USB or the network, and never finds out which it has. Cursor
 * addressing, colour and attributes all go through setstat: the panel moves
 * a render position and paints cells, while a terminal is handed the escape
 * sequences SCF makes for it.
 *
 * Which is the point. A console app should be written once.
 */
#include "modlib.h"

static void bar(const rv9_mod_env_t *env, int p, uint32_t row, uint32_t cols,
                const char *title)
{
    m_cursor(env, p, row, 0);
    m_colour(env, p, RV9_COL_BLACK, RV9_COL_CYAN);
    m_attr(env, p, RV9_CON_ATTR_BOLD);

    uint32_t n = m_len(title);
    m_say(env, p, " ");
    m_say(env, p, title);
    for (uint32_t i = n + 1; i < cols; i++) m_say(env, p, " ");

    m_attr(env, p, 0);
    m_colour(env, p, RV9_COL_DEFAULT, RV9_COL_DEFAULT);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 12) return -1;

    /* Default to stdout, so `screen` works over a pipe or the network
       shell and `screen /term` puts the same picture on the glass. */
    int p = RV9_STDOUT;
    bool opened = false;

    if (env->arg && env->arg[0] && env->arg[0] != ' ') {
        char name[32];
        m_word(env->arg, name, sizeof(name));
        p = env->open(name, RV9_MODE_WRITE);
        if (p < 0) {
            m_say(env, RV9_STDOUT, "screen: cannot open ");
            m_say(env, RV9_STDOUT, name);
            m_say(env, RV9_STDOUT, "\n");
            return -2;
        }
        opened = true;
    }

    uint32_t rows = 0, cols = 0;
    m_screen(env, p, &rows, &cols);

    m_clear(env, p, RV9_CON_CLEAR_SCREEN);
    bar(env, p, 0, cols, "RV-9 console");

    /*
     * Everything below lays out against the size the device reported, and
     * draws a section only if there is room for it.
     *
     * That is not defensive padding, it is the demonstration: the panel is
     * 30x8 and a terminal is usually 80x24, and a program written for one
     * should not produce nonsense on the other. Asking is the whole reason
     * RV9_CON_GS_SIZE exists.
     */
    uint32_t row = 2;

    /* Sixteen foregrounds, four columns each, wrapped to the width. */
    uint32_t per_row = cols / 4;
    if (per_row < 1) per_row = 1;
    uint32_t swatch_rows = (16 + per_row - 1) / per_row;

    if (row + swatch_rows <= rows) {
        uint32_t col = 0;
        for (uint32_t i = 0; i < 16; i++) {
            if (col + 4 > cols) { col = 0; row++; }
            m_cursor(env, p, row, col);
            m_colour(env, p, i, RV9_COL_DEFAULT);
            m_say(env, p, "##");
            m_numpad(env, p, (int32_t)i, 2);
            col += 4;
        }
        m_colour(env, p, RV9_COL_DEFAULT, RV9_COL_DEFAULT);
        row += 2;
    }

    if (row < rows) {
        m_cursor(env, p, row, 0);
        m_say(env, p, "plain ");
        m_attr(env, p, RV9_CON_ATTR_BOLD);
        m_say(env, p, "bold ");
        m_attr(env, p, RV9_CON_ATTR_UNDERLINE);
        m_say(env, p, "under ");
        m_attr(env, p, RV9_CON_ATTR_REVERSE);
        m_say(env, p, "rev");
        m_attr(env, p, 0);
        row += 2;
    }

    /* A box drawn by moving the cursor, which is the thing that does not
       work at all without any of this. */
    if (row + 3 <= rows) {
        uint32_t bw = (cols < 20) ? cols : 20;
        m_colour(env, p, RV9_COL_WHITE | RV9_COL_BRIGHT, RV9_COL_BLUE);
        for (uint32_t i = 0; i < bw; i++) {
            m_cursor(env, p, row, i);
            m_say(env, p, " ");
            m_cursor(env, p, row + 2, i);
            m_say(env, p, " ");
        }
        m_cursor(env, p, row + 1, 0);
        m_say(env, p, " ");
        m_cursor(env, p, row + 1, bw - 1);
        m_say(env, p, " ");
        m_colour(env, p, RV9_COL_DEFAULT, RV9_COL_DEFAULT);

        m_cursor(env, p, row + 1, 2);
        m_say(env, p, (bw >= 18) ? "drawn by address" : "by address");
        row += 4;
    }

    /* Say what we were told, on the last line that exists. */
    uint32_t last = (row < rows) ? row : rows - 1;
    m_cursor(env, p, last, 0);
    m_num(env, p, (int32_t)rows);
    m_say(env, p, " rows x ");
    m_num(env, p, (int32_t)cols);
    m_say(env, p, " cols");
    if (last + 1 < rows) m_say(env, p, "\n");

    if (opened) env->close(p);
    return 0;
}
