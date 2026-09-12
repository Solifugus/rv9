/*
 * passwd -- set the login password, and show the host key.
 *
 *   passwd <password>     set it
 *   passwd show           the host key fingerprint, and whether one is set
 *
 * Typed rather than stored anywhere in the source tree, like the WiFi
 * credentials and for the same reason. It goes to /sshcfg, which is the
 * SSH driver bound so that opening it does not wait for a login -- the
 * device you would otherwise have to log in to in order to set the
 * password you need in order to log in.
 */
#include "modlib.h"
#include "rv9/sshd.h"

typedef struct {
    rv9_ssh_pw_t   pw;
    rv9_ssh_info_t info;
} passwd_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    passwd_statics_t *st = (passwd_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: passwd <password>\n"
                               "       passwd show\n");
        return -3;
    }

    int p = env->open("/sshcfg", RV9_MODE_RW);
    if (p < 0) { m_say(env, RV9_STDOUT, "/sshcfg: cannot open\n"); return -4; }

    if (m_eq(env->arg, "show")) {
        int rc = env->getstat(p, RV9_SSH_GS_INFO, &st->info);
        env->close(p);
        if (rc < 0) { m_say(env, RV9_STDOUT, "cannot read\n"); return -5; }

        m_say(env, RV9_STDOUT, "host key   ");
        m_say(env, RV9_STDOUT, st->info.fingerprint);
        m_say(env, RV9_STDOUT, "\npassword   ");
        m_say(env, RV9_STDOUT, st->info.have_password ? "set\n" : "NOT SET\n");

        if (st->info.connected) {
            m_say(env, RV9_STDOUT, "session    ");
            m_say(env, RV9_STDOUT, st->info.user);
            if (st->info.have_pty) {
                m_say(env, RV9_STDOUT, ", ");
                m_num(env, RV9_STDOUT, st->info.cols);
                m_say(env, RV9_STDOUT, "x");
                m_num(env, RV9_STDOUT, st->info.rows);
            }
            m_say(env, RV9_STDOUT, "\n");
        }
        return 0;
    }

    uint32_t i = 0;
    while (env->arg[i] && i < sizeof(st->pw.pass) - 1) {
        st->pw.pass[i] = env->arg[i];
        i++;
    }
    st->pw.pass[i] = '\0';

    int rc = env->setstat(p, RV9_SSH_SS_PASSWORD, &st->pw);
    env->close(p);

    /* The driver wipes its copy; wipe ours too, so the password is not
       left sitting in a module's static area for the next reader of
       /proc-style output to find. */
    for (uint32_t j = 0; j < sizeof(st->pw.pass); j++) st->pw.pass[j] = 0;

    if (rc < 0) { m_say(env, RV9_STDOUT, "could not set the password\n"); return -6; }

    /* sshd starts itself at boot; it only needed a password to exist. */
    m_say(env, RV9_STDOUT, "password set; saved to flash\n"
                           "reset the board, or run `sshd`, to serve it\n");
    return 0;
}
