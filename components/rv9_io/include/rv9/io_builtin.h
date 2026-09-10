/*
 * Registration for the file managers and drivers built into the kernel.
 *
 * These are separate modules in the OS-9 sense but are compiled in for now:
 * the module ABI cannot yet express what a driver needs (register access,
 * interrupts, DMA). Making them loadable is a later phase; the interfaces
 * in rv9/io.h are already shaped for it, so nothing above has to change.
 *
 * Device *descriptors* are already real loadable modules -- see
 * rv9_io_attach_from_modules().
 */
#pragma once

#include "rv9/io.h"

rv9_io_err_t rv9_scf_register(void);
rv9_io_err_t rv9_drv_uart_register(void);
rv9_io_err_t rv9_drv_lcdcon_register(void);
rv9_io_err_t rv9_rbf_register(void);
rv9_io_err_t rv9_drv_ramdisk_register(void);
