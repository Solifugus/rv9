/*
 * SSH device codes, shared by the driver and the utilities that configure
 * it. Driver-private, hence above RV9_SS_DRIVER_BASE.
 *
 * Two devices come off this one driver, and the descriptor says which:
 *
 *   /ssh0     a session. Opening it waits for somebody to connect, prove
 *             who they are, and ask for a shell.
 *   /sshcfg   the same driver with nothing behind it, for setting the
 *             password and reading the host key fingerprint.
 *
 * The second exists because the first blocks. A password that could only
 * be set through a device that waits for a login would be a password you
 * could never set the first time.
 */
#pragma once

#include <stdint.h>
#include "rv9/module.h"

#define RV9_SSH_SS_PASSWORD  (RV9_SS_DRIVER_BASE + 16)   /* rv9_ssh_pw_t   */
#define RV9_SSH_GS_INFO      (RV9_SS_DRIVER_BASE + 17)   /* rv9_ssh_info_t */

typedef struct __attribute__((packed)) {
    char pass[65];
} rv9_ssh_pw_t;

typedef struct __attribute__((packed)) {
    char     fingerprint[56];   /* "SHA256:" and 43 base64 characters */
    char     user[32];          /* who is logged in, "" if nobody */
    uint8_t  have_password;
    uint8_t  connected;
    uint8_t  have_pty;
    uint8_t  reserved;
    uint16_t cols, rows;
} rv9_ssh_info_t;
