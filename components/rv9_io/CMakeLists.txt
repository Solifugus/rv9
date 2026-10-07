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
# The glass.
#
# Everything above the panel is portable and is built for both boards: the
# console, the font, the scanline rasteriser and the SVG window. Only the
# panel underneath them is board-specific, and the two are not alike --
#
#   panel.c      an ST7789 on SPI, which holds its own framebuffer and is
#                written a window at a time. The microSD card shares its
#                bus, so drv_sdspi.c goes with it.
#
# The card is the same shape: one driver name, `sdcard`, and the board
# decides whether that is SPI (drv_sdspi.c) or four-line SDMMC
# (drv_sdmmc.c). A descriptor names a driver and there is one module store
# for both boards, so the name cannot carry the transport.
#   panel_dsi.c  an EK79007 on MIPI-DSI, which holds nothing: the P4 scans
#                the picture out of PSRAM continuously, so there is a real
#                1.2 MB framebuffer and a blit is a copy into it.
#
# They answer the same panel.h, which is what makes the layers above them
# the same code. The one thing that was NOT portable turned out to be in
# drv_lcdcon.c, which had the C5's 172x320 written into it while claiming
# to ask the panel -- see the note there.
#
list(APPEND RV9_IO_SRCS "src/drv_lcdcon.c" "src/font.c" "src/raster.c"
                        "src/drv_svgwin.c" "src/drv_touch.c")
list(APPEND RV9_IO_REQ esp_lcd esp_hw_support)

if("${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND RV9_IO_SRCS "src/panel_dsi.c" "src/drv_sdmmc.c")
    list(APPEND RV9_IO_REQ esp_driver_sdmmc sdmmc)
else()
    list(APPEND RV9_IO_SRCS "src/panel.c" "src/drv_sdspi.c")
    list(APPEND RV9_IO_REQ esp_driver_sdspi sdmmc)
endif()

idf_component_register(SRCS ${RV9_IO_SRCS}
                       INCLUDE_DIRS "include"
                       REQUIRES ${RV9_IO_REQ})
