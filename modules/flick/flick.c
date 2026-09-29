/*
 * flick -- how long a tap's acknowledgement actually takes.
 *
 *     flick > /w0        a button drawn, flashed, and drawn again
 *
 * The question this answers is whether tap-and-flash feedback is affordable
 * on this hardware. A whole-screen redraw is not: at 1024x600 it is about
 * 640 ms on a C5-class core (design §56). A strip is, and this measures the
 * difference on the real board rather than extrapolating from a host.
 *
 * Three draws, timed by the caller reading the log:
 *
 *   1. the whole window, once, as the baseline
 *   2. the button's rows only, in its pressed colour
 *   3. the button's rows only, back to normal
 *
 * 2 and 3 together are what a person experiences as one tap. They are
 * clipped with RV9_SVG_SS_ROWS, which is why they cost a fraction of 1.
 *
 * The button is drawn at the same place by the same SVG both times, with
 * only its fill changed -- because the point is to measure the redraw, not
 * to demonstrate that a rectangle can be a different colour.
 */
#include "modlib.h"

/* Where the button lives. Rows chosen to sit on band boundaries so the clip
   costs exactly the bands the button occupies and not one more. */
#define BTN_Y0   96
#define BTN_Y1   144

static const char *SCENE =
    "<svg viewBox=\"0 0 320 172\">"
      "<rect x=\"0\" y=\"0\" width=\"320\" height=\"172\" fill=\"#10203a\"/>"
      "<text x=\"12\" y=\"20\" font-size=\"10\" fill=\"#cfe\">tap test</text>"
      "<rect x=\"40\" y=\"96\" width=\"240\" height=\"48\" fill=\"#2a6b8f\"/>"
      "<text x=\"96\" y=\"126\" font-size=\"12\" fill=\"#fff\">CONFIRM</text>"
    "</svg>";

/* The same button, pressed. Only the fill differs. */
static const char *PRESSED =
    "<svg viewBox=\"0 0 320 172\">"
      "<rect x=\"40\" y=\"96\" width=\"240\" height=\"48\" fill=\"#8fd0ff\"/>"
      "<text x=\"96\" y=\"126\" font-size=\"12\" fill=\"#123\">CONFIRM</text>"
    "</svg>";

static const char *NORMAL =
    "<svg viewBox=\"0 0 320 172\">"
      "<rect x=\"40\" y=\"96\" width=\"240\" height=\"48\" fill=\"#2a6b8f\"/>"
      "<text x=\"96\" y=\"126\" font-size=\"12\" fill=\"#fff\">CONFIRM</text>"
    "</svg>";

/* Clip the next document to the button's rows. */
static int clip_rows(const rv9_mod_env_t *env, uint32_t y0, uint32_t y1)
{
    uint32_t v = (y0 << 16) | (y1 & 0xFFFF);
    return env->setstat(RV9_STDOUT, RV9_SVG_SS_ROWS, &v);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 11) return 1;   /* time_us */

    uint64_t t0 = env->time_us();
    m_say(env, RV9_STDOUT, SCENE);
    uint32_t full = (uint32_t)(env->time_us() - t0);

    /*
     * If the device will not clip, say so and stop rather than reporting a
     * strip time that is really another full redraw. A number that quietly
     * measures the wrong thing is worse than no number.
     */
    if (clip_rows(env, BTN_Y0, BTN_Y1) < 0) {
        m_say(env, RV9_STDERR, "flick: this window cannot clip rows; "
                               "send it to /w0\n");
        return 2;
    }

    t0 = env->time_us();
    m_say(env, RV9_STDOUT, PRESSED);
    uint32_t press = (uint32_t)(env->time_us() - t0);

    env->sleep_ms(60);                    /* long enough for an eye */

    clip_rows(env, BTN_Y0, BTN_Y1);
    t0 = env->time_us();
    m_say(env, RV9_STDOUT, NORMAL);
    uint32_t release = (uint32_t)(env->time_us() - t0);

    /* To stderr, which is not the window: writing the answer into the
       picture would redraw the thing being measured. */
    m_say(env, RV9_STDERR, "flick: full ");
    m_num(env, RV9_STDERR, (int32_t)(full / 1000));
    m_say(env, RV9_STDERR, " ms, press ");
    m_num(env, RV9_STDERR, (int32_t)(press / 1000));
    m_say(env, RV9_STDERR, " ms, release ");
    m_num(env, RV9_STDERR, (int32_t)(release / 1000));
    m_say(env, RV9_STDERR, " ms (a tap is press+release)\n");
    return 0;
}
