/*
 * uart driver -- characters to the host console.
 *
 * On this board the console is the C5's native USB-Serial/JTAG, reached
 * through the host kernel's stdout. That is a shim, and an honest one: when
 * RV-9 owns the hardware in phase 7, this file talks to the peripheral
 * directly and nothing above it changes.
 */
#include "rv9/io.h"

#include <stdio.h>

static rv9_io_err_t uart_init(rv9_dev_t *dev)
{
    (void)dev;
    return RV9_IO_OK;
}

static rv9_io_err_t uart_write(rv9_dev_t *dev, const void *buf, size_t len,
                               size_t *done)
{
    (void)dev;

    size_t n = fwrite(buf, 1, len, stdout);
    fflush(stdout);

    if (done) *done = n;
    return (n == len) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

/*
 * TODO (phase 4): the shell needs input. Non-blocking console reads mean
 * configuring the VFS for the USB-Serial/JTAG driver; deferred until there
 * is something to type at.
 */
static rv9_io_err_t uart_read(rv9_dev_t *dev, void *buf, size_t len,
                              size_t *done)
{
    (void)dev; (void)buf; (void)len;
    if (done) *done = 0;
    return RV9_IO_ERR_WOULDBLOCK;
}

static const rv9_driver_t uart = {
    .name  = "uart",
    .init  = uart_init,
    .read  = uart_read,
    .write = uart_write,
};

rv9_io_err_t rv9_drv_uart_register(void)
{
    return rv9_io_register_driver(&uart);
}
