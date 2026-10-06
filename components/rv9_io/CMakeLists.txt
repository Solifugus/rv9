# Everything that does not depend on which chip this is.
set(RV9_IO_SRCS "src/io.c" "src/claim.c" "src/scf.c" "src/drv_uart.c"
                "src/rbf.c" "src/drv_ramdisk.c"
                "src/drv_flashdisk.c"
                "src/pio.c" "src/drv_gpio.c" "src/console.c" "src/drv_pwm.c"
                "src/drv_adc.c" "src/drv_tsens.c"
                "src/pfm.c" "src/drv_pubmem.c"
                "src/pipefm.c" "src/drv_pipemem.c"
                "src/ifm.c" "src/drv_i2c.c")

set(RV9_IO_REQ rv9_kal rv9_module rv9_proc driver esp_driver_i2c
               esp_driver_usb_serial_jtag esp_timer
               nvs_flash esp_driver_ledc esp_driver_gpio esp_adc
               esp_driver_tsens)

#
# The radio, where there is one.
#
# The ESP32-P4 has none at all -- its WiFi comes from a companion ESP32-C6 over
# SDIO, through components that are not in this IDF install. esp_wifi's own
# header does not compile for a chip with no radio: CONFIG_ESP_WIFI_* simply
# does not exist, so drv_net.c cannot be built, which means /n0 cannot exist.
#
# That is not a gap to apologise for, it is the device model working. A device
# exists because a descriptor says so; no driver, no descriptor, no device.
# `have net` answers no, and a program declaring devices="/n0" is refused at
# fork rather than part-way through. Nothing above this line has to know.
#
if(NOT "${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND RV9_IO_SRCS "src/nfm.c" "src/drv_net.c")
    list(APPEND RV9_IO_REQ lwip esp_netif esp_wifi esp_event)
else()
    # The console is a real UART here, behind the board's CH343 bridge --
    # see the header of src/drv_uart.c, which is a USB Serial/JTAG driver
    # everywhere else despite its name.
    list(APPEND RV9_IO_REQ esp_driver_uart)
endif()

#
# The glass, where it is the glass RV-9 knows about.
#
# panel.c is an ST7789 driven over SPI on the C5's pin numbers. This P4 board
# has a 1024x600 MIPI-DSI panel on none of them, and -- the part that is not
# just pins -- a DSI panel has no framebuffer of its own, so "whoever painted
# last is what you see" stops being true and RV-9 would own 1.2 MB of pixels
# in PSRAM instead. That is a different panel.c, not a port of this one.
#
# So /term and /w0 do not exist here yet, and the same rule applies as to the
# radio: no driver, no descriptor, no device. The console is /uart0, which on
# this board is a real UART and works.
#
# sdspi goes with it: it shares the panel's SPI bus and its pins are the C5's.
#
if(NOT "${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND RV9_IO_SRCS "src/drv_lcdcon.c" "src/font.c" "src/panel.c"
                            "src/raster.c" "src/drv_svgwin.c" "src/drv_sdspi.c")
    list(APPEND RV9_IO_REQ esp_lcd esp_driver_sdspi sdmmc)
endif()

idf_component_register(SRCS ${RV9_IO_SRCS}
                       INCLUDE_DIRS "include"
                       REQUIRES ${RV9_IO_REQ})
