/*
 * pic -- a picture, for proving the window device works.
 *
 *   pic > /w0     draw it on the panel
 *   pic           print the SVG instead
 *
 * It is an SVG document and nothing else, which is the whole demonstration:
 * the program does not know what a panel is, does not link a graphics
 * library, and would send the same bytes to a file or down a socket. The
 * viewBox is what makes it fit a 320x172 strip without being written for
 * one.
 */
#include "modlib.h"

static const char *PICTURE =
    "<svg viewBox=\"0 0 320 172\">"

      /* Sky and ground, as two rectangles. */
      "<rect x=\"0\" y=\"0\" width=\"320\" height=\"120\" fill=\"#102040\"/>"
      "<rect x=\"0\" y=\"120\" width=\"320\" height=\"52\" fill=\"#1d5c2e\"/>"

      /* Sun, with a ring around it to show strokes and transparency order. */
      "<circle cx=\"258\" cy=\"40\" r=\"22\" fill=\"#ffcc33\"/>"
      "<circle cx=\"258\" cy=\"40\" r=\"30\" fill=\"none\""
             " stroke=\"#ffcc33\" stroke-width=\"2\"/>"

      /* Hills: filled polygons. */
      "<polygon points=\"-10,120 70,58 150,120\" fill=\"#2a6b3f\"/>"
      "<polygon points=\"90,120 180,44 280,120\" fill=\"#225c37\"/>"

      /* A group, translated, so inheritance and transform both get used. */
      "<g transform=\"translate(24,96)\" stroke=\"#20303a\" stroke-width=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"46\" height=\"40\" fill=\"#c8b28a\"/>"
        "<polygon points=\"-6,0 23,-20 52,0\" fill=\"#8c3b2e\"/>"
        "<rect x=\"16\" y=\"18\" width=\"14\" height=\"22\" fill=\"#5a3a24\"/>"
      "</g>"

      /* An ellipse and a couple of lines, to exercise the rest. */
      "<ellipse cx=\"150\" cy=\"150\" rx=\"60\" ry=\"9\" fill=\"#164a26\"/>"
      "<line x1=\"0\" y1=\"120\" x2=\"320\" y2=\"120\""
            " stroke=\"#0d3a1c\" stroke-width=\"2\"/>"
      "<polyline points=\"8,164 40,150 72,160 104,142 136,156\""
               " fill=\"none\" stroke=\"#7fd18f\" stroke-width=\"2\"/>"

    "</svg>";

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    m_say(env, RV9_STDOUT, PICTURE);
    return 0;
}
