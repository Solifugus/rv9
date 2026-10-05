# Everything that does not depend on which chip this is.
set(RV9_IO_SRCS "src/io.c" "src/claim.c" "src/scf.c" "src/drv_uart.c"
                "src/drv_lcdcon.c" "src/font.c"
                "src/rbf.c" "src/drv_ramdisk.c" "src/drv_sdspi.c"
                "src/drv_flashdisk.c"
                "src/pio.c" "src/drv_gpio.c" "src/console.c" "src/drv_pwm.c"
                "src/panel.c" "src/raster.c" "src/drv_svgwin.c"
                "src/drv_adc.c" "src/drv_tsens.c"
                "src/pfm.c" "src/drv_pubmem.c"
                "src/pipefm.c" "src/drv_pipemem.c"
                "src/ifm.c" "src/drv_i2c.c")

set(RV9_IO_REQ rv9_kal rv9_module rv9_proc esp_lcd driver esp_driver_i2c
               esp_driver_sdspi sdmmc esp_driver_usb_serial_jtag esp_timer
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

idf_component_register(SRCS ${RV9_IO_SRCS}
                       INCLUDE_DIRS "include"
                       REQUIRES ${RV9_IO_REQ})
