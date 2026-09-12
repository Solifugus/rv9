/*
 * chart -- a chart, for proving the window device draws charts.
 *
 *   chart > /w0     draw it on the panel
 *   chart           print the SVG instead
 *
 * Between them these cover what a chart is made of: gridlines and axes as
 * straight paths, a filled area, a smooth series in cubic curves, a donut
 * with a real hole, and labels -- axis values right-aligned outside the
 * axis, month names centred under their ticks, both inheriting size and
 * alignment from the group around them.
 *
 * Nothing here knows what a panel is. Redirect it to a file and the same
 * bytes go there.
 */
#include "modlib.h"

static const char *PICTURE =
    "<svg viewBox=\"0 0 320 172\"><rect x=\"0\" y=\"0\" width=\"320\" height=\"172"
    "\" fill=\"#f4f1ea\"/><g stroke=\"#c9c2b4\" stroke-width=\"1\" fill=\"none\"><"
    "path d=\"M34 20 H210 M34 50 H210 M34 80 H210 M34 110 H210\"/></g><path"
    " d=\"M34 140 L34 14 M34 140 L210 140\" stroke=\"#5a5346\" stroke-width=\""
    "1\" fill=\"none\"/><path d=\"M34 118 L64 96 L94 104 L124 62 L154 70 L184"
    " 34 L210 46 L210 140 L34 140 Z\" fill=\"#9fc7e8\"/><path d=\"M34 118 C 4"
    "9 96, 54 92, 64 96 S 114 60, 124 62 S 174 30, 210 46\" fill=\"none\" st"
    "roke=\"#1f6fb2\" stroke-width=\"2\"/><g fill=\"#5a5346\" font-size=\"11\" te"
    "xt-anchor=\"end\"><text x=\"30\" y=\"24\">80</text><text x=\"30\" y=\"54\">60<"
    "/text><text x=\"30\" y=\"84\">40</text><text x=\"30\" y=\"114\">20</text></g"
    "><g fill=\"#5a5346\" font-size=\"11\" text-anchor=\"middle\"><text x=\"34\" "
    "y=\"154\">Jan</text><text x=\"94\" y=\"154\">Mar</text><text x=\"154\" y=\"15"
    "4\">May</text><text x=\"210\" y=\"154\">Jul</text></g><text x=\"34\" y=\"12\""
    " fill=\"#1f6fb2\" font-size=\"13\">Revenue</text><path d=\"M268 70 m -34 "
    "0 a 34 34 0 1 0 68 0 a 34 34 0 1 0 -68 0 Z M268 70 m -15 0 a 15 15 0"
    " 1 1 30 0 a 15 15 0 1 1 -30 0 Z\" fill=\"#2f9e6d\" fill-rule=\"evenodd\"/"
    "><text x=\"268\" y=\"150\" fill=\"#2f9e6d\" font-size=\"12\" text-anchor=\"mi"
    "ddle\">61% share</text></svg>";

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    m_say(env, RV9_STDOUT, PICTURE);
    return 0;
}
