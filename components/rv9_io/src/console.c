/*
 * Console settings, rendered as escape sequences.
 *
 * A device that has its own idea of what a cursor is -- the LCD console,
 * which owns a character grid and a font renderer -- answers the console
 * setstats itself. Everything else has a terminal at the far end of a wire,
 * and a terminal is driven by putting bytes in the stream.
 *
 * So this is the fallback: turn one setstat into the escape sequence that
 * means it. It lives here rather than in SCF because the network file
 * manager needs the same translation for the same reason -- a shell over
 * TCP is talking to somebody's terminal too -- and two copies of a table of
 * escape codes is one copy too many.
 *
 * The sequences are ECMA-48, which is what every terminal emulator in use
 * implements whatever it calls itself.
 */
#include "rv9/io.h"

#include <stdio.h>

/*
 * Colour, as a parameter number.
 *
 * The palette order is the VT100's, so the arithmetic is: 30 + colour for
 * foreground, 40 + colour for background, and the bright half moves to the
 * 90s and 100s. RV9_COL_DEFAULT is 39 and 49, which is the terminal's own
 * idea rather than a colour we picked.
 */
static int sgr_colour(uint32_t c, bool background)
{
    int base = background ? 40 : 30;

    if (c == RV9_COL_DEFAULT) return base + 9;
    if (c & RV9_COL_BRIGHT)   return base + 60 + (int)(c & 7);
    return base + (int)(c & 7);
}

size_t rv9_con_ansi(char *buf, size_t cap, uint32_t code, uint32_t value)
{
    int n = 0;

    switch (code) {
    case RV9_CON_SS_CURSOR:
        /* Rows and columns are 0-based in RV-9 and 1-based in ECMA-48.
           Off-by-one bugs in cursor addressing are miserable to find, so
           the conversion happens in exactly this one place. */
        n = snprintf(buf, cap, "\033[%u;%uH",
                     (unsigned)((value >> 16) & 0xFFFF) + 1,
                     (unsigned)(value & 0xFFFF) + 1);
        break;

    case RV9_CON_SS_COLOUR:
        n = snprintf(buf, cap, "\033[%d;%dm",
                     sgr_colour(value & 0xFF, false),
                     sgr_colour((value >> 8) & 0xFF, true));
        break;

    case RV9_CON_SS_ATTR:
        /*
         * Every attribute, on or off, every time -- never a bare reset.
         *
         * SGR 0 would be shorter and would also clear the colour, which the
         * caller did not ask for and did not expect. Saying all three
         * explicitly makes this setstat mean only what it says.
         */
        n = snprintf(buf, cap, "\033[%d;%d;%dm",
                     (value & RV9_CON_ATTR_BOLD)      ? 1 : 22,
                     (value & RV9_CON_ATTR_UNDERLINE) ? 4 : 24,
                     (value & RV9_CON_ATTR_REVERSE)   ? 7 : 27);
        break;

    case RV9_CON_SS_CLEAR:
        if (value == RV9_CON_CLEAR_EOL)      n = snprintf(buf, cap, "\033[K");
        else if (value == RV9_CON_CLEAR_EOS) n = snprintf(buf, cap, "\033[J");
        else                                 n = snprintf(buf, cap, "\033[2J\033[H");
        break;

    case RV9_CON_SS_CURSOR_ON:
        n = snprintf(buf, cap, value ? "\033[?25h" : "\033[?25l");
        break;

    default:
        return 0;
    }

    /* A truncated escape sequence is worse than none: the terminal would
       swallow whatever text came next looking for the end of it. */
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}
