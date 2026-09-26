/*
 * st-watch-mm -- a watcher that agrees with sonar about millimetres.
 *
 * One of a pair. This one is admitted; st-watch-m is refused, and the pair
 * exists so that the refusal is demonstrated against a passing control
 * rather than asserted on its own.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL) return 1;
    m_say(env, RV9_STDOUT, "watching millimetres\n");
    return 0;
}
