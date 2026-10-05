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

/*
 * TWO CONSOLES, ONE DEVICE NAME
 *
 * `/uart0` is what the console is called, not what it is. On the ESP32-C5 it
 * is the chip's own USB Serial/JTAG peripheral -- the programming cable *is*
 * the console, and there is no bridge chip. On this ESP32-P4 board the console
 * goes out through a WCH CH343 wired to UART0, and the USB Serial/JTAG
 * peripheral is a different connector.
 *
 * So the device keeps its name and its behaviour, and swaps what it talks to.
 * Nothing above this file changes: SCF still sees a character device, the
 * shell still reads stdin, and `/uart0` is still where the boot log goes.
 *
 * Keyed on the target rather than on CONFIG_ESP_CONSOLE_*, because the
 * question is which peripheral the board wired to the socket, and the board
 * is what the target stands for here.
 */
#if CONFIG_IDF_TARGET_ESP32P4
#  include "driver/uart.h"
#  define CONSOLE_UART  UART_NUM_0
#else
#  include "driver/usb_serial_jtag.h"
#  include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_log.h"

static const char *TAG = "rv9-uart";

#define RX_BUF 256
#define TX_BUF 256

static bool s_installed;

static rv9_io_err_t uart_init(rv9_dev_t *dev)
{
    (void)dev;

    if (s_installed) return RV9_IO_OK;

#if CONFIG_IDF_TARGET_ESP32P4
    /* The ROM bootloader already configured these pins and this baud, which
       is how its output reached the cable; take them as they are. */
    if (uart_driver_install(CONSOLE_UART, RX_BUF, TX_BUF, 0, NULL, 0)
            != ESP_OK) {
        ESP_LOGE(TAG, "could not install the UART driver");
        return RV9_IO_ERR_IO;
    }
    s_installed = true;
    ESP_LOGI(TAG, "console on UART%d, through the board's USB bridge",
             CONSOLE_UART);
#else
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
#endif
    return RV9_IO_OK;
}

static rv9_io_err_t uart_write(rv9_dev_t *dev, const void *buf, size_t len,
                               size_t *done)
{
    (void)dev;

#if CONFIG_IDF_TARGET_ESP32P4
    int n = uart_write_bytes(CONSOLE_UART, buf, len);
#else
    int n = usb_serial_jtag_write_bytes(buf, len, rv9_ms_to_ticks(100));
#endif
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

#if CONFIG_IDF_TARGET_ESP32P4
    int n = uart_read_bytes(CONSOLE_UART, buf, len, 0);   /* never waits */
#else
    int n = usb_serial_jtag_read_bytes(buf, len, 0);   /* never waits */
#endif
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
