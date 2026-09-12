/*
 * Registration for the SSH driver, kept apart from rv9/sshd.h so that
 * modules -- which have no libc and must not see the I/O manager's own
 * types -- can include the codes without this.
 */
#pragma once

#include "rv9/io.h"

rv9_io_err_t rv9_drv_ssh_register(void);
