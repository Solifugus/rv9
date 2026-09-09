/*
 * uart driver -- the console on the C5's native USB Serial/JTAG.
 *
 * This is the board's only terminal: the same cable that flashes it carries
 * keystrokes in and characters out. There is no host controller on this
 * chip, so a USB keyboard is not an option; the terminal is whatever is on
 * the other end of the cable.
 *
 * Reads are non-blocking by design. Blocking until a line arrives is the
 * file manager's job, not the driver's -- SCF owns line discipline, this
 * file owns bytes.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"

static const char *TAG = "rv9-uart";

#define RX_BUF 256
#define TX_BUF 256

static bool s_installed;

static rv9_io_err_t uart_init(rv9_dev_t *dev)
{
    (void)dev;

    if (s_installed) return RV9_IO_OK;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = TX_BUF,
        .rx_buffer_size = RX_BUF,
    };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "could not install USB Serial/JTAG driver");
        return RV9_IO_ERR_IO;
    }

    /* Route the host kernel's stdio through the same driver, so RV-9's
       output and the boot log do not fight over the peripheral. */
    usb_serial_jtag_vfs_use_driver();

    s_installed = true;
    ESP_LOGI(TAG, "console on USB Serial/JTAG");
    return RV9_IO_OK;
}

static rv9_io_err_t uart_write(rv9_dev_t *dev, const void *buf, size_t len,
                               size_t *done)
{
    (void)dev;

    int n = usb_serial_jtag_write_bytes(buf, len, rv9_ms_to_ticks(100));
    if (n < 0) {
        if (done) *done = 0;
        return RV9_IO_ERR_IO;
    }

    if (done) *done = (size_t)n;
    return ((size_t)n == len) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

static rv9_io_err_t uart_read(rv9_dev_t *dev, void *buf, size_t len,
                              size_t *done)
{
    (void)dev;

    int n = usb_serial_jtag_read_bytes(buf, len, 0);   /* never waits */
    if (n < 0) {
        if (done) *done = 0;
        return RV9_IO_ERR_IO;
    }

    if (done) *done = (size_t)n;
    return (n > 0) ? RV9_IO_OK : RV9_IO_ERR_WOULDBLOCK;
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
