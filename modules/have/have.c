/*
 * have -- does this machine have it?
 *
 *     have /i2c0        a device, by name
 *     have svgwin       a driver: is there anything I can draw on?
 *     have scf          a file manager: is there a character device?
 *     have              what a program would usually ask about
 *
 * The point is not the command, which is a convenience. The point is that
 * one program can run on a small board and a large one by *requiring* only
 * what is essential -- declared in its manifest, so admission refuses it
 * before it runs -- and *asking* about the rest.
 *
 * Exit status is the answer, so a script can use it:
 *
 *     have i2c && rt sonar
 */
#include "modlib.h"

/*
 * The ones worth reporting when asked nothing in particular -- a capability
 * each, rather than a device each.
 *
 * One string walked at runtime, not an array of pointers. An array of string
 * pointers needs relocation entries and is therefore not position
 * independent: the build refuses it, which is the good case. The same trap
 * caught `dump`. A pointer to a literal is fine; a table of them is not.
 *
 *   svgwin  somewhere to draw      net    a network
 *   lcdcon  a panel console        sdspi  a card
 *   i2c     a two-wire bus         pwm    an actuator
 *   adc     an analogue input      ssh    remote login
 */
#define USUAL "svgwin lcdcon i2c net sdspi pwm adc ssh"

static void report(const rv9_mod_env_t *env, const char *what)
{
    rv9_sys_device_t d;
    bool yes = m_have(env, what, &d);

    m_pad(env, RV9_STDOUT, what, 10);
    if (!yes) {
        m_say(env, RV9_STDOUT, "-\n");
        return;
    }
    m_pad(env, RV9_STDOUT, d.name, 10);
    m_pad(env, RV9_STDOUT, d.filemgr, 7);
    m_say(env, RV9_STDOUT, d.driver);
    m_say(env, RV9_STDOUT, "\n");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 5) return 2;   /* sysinfo */

    if (env->arg != NULL && env->arg[0] != '\0') {
        char what[16];
        m_word(env->arg, what, sizeof(what));

        /*
         * Silent, with the answer in the status, so it composes:
         * `have i2c && rt sonar`. Printing "yes" would make a script parse
         * text to learn what the exit code already said.
         */
        return m_have(env, what, NULL) ? 0 : 1;
    }

    m_say(env, RV9_STDOUT, "asked     device    mgr    driver\n");

    const char *p = USUAL;
    while (*p != '\0') {
        char what[16];
        p = m_word(p, what, sizeof(what));
        if (what[0] != '\0') report(env, what);
    }
    return 0;
}
