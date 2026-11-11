# Everything that does not depend on which chip this is, and that the
# system uses itself: the console, /f0, publication, pipes, and /m0, the
# modules it holds as files.
set(RV9_IO_SRCS "src/io.c" "src/claim.c" "src/scf.c" "src/drv_uart.c"
                "src/rbf.c" "src/drv_flashdisk.c" "src/console.c"
                "src/pfm.c" "src/drv_pubmem.c"
                "src/pipefm.c" "src/drv_pipemem.c" "src/mfm.c")

#
# The rest is chosen, one device at a time (Kconfig, "RV-9 devices"): a
# board without a camera, or a system that does not want one, leaves its
# driver out of the firmware altogether. Shared pieces come in with
# whatever needs them.
#
macro(rv9_if opt)
    if(${opt})
        list(APPEND RV9_IO_SRCS ${ARGN})
    endif()
endmacro()

rv9_if(CONFIG_RV9_DRV_RAMDISK "src/drv_ramdisk.c")
rv9_if(CONFIG_RV9_DRV_GPIO    "src/drv_gpio.c")
rv9_if(CONFIG_RV9_DRV_PWM     "src/drv_pwm.c")
rv9_if(CONFIG_RV9_DRV_ADC     "src/drv_adc.c")
rv9_if(CONFIG_RV9_DRV_TSENS   "src/drv_tsens.c")
rv9_if(CONFIG_RV9_DRV_AUDIO   "src/drv_audio.c")
rv9_if(CONFIG_RV9_DRV_I2C     "src/ifm.c")

# The file manager behind /gpio, /pwm0, /adc0 and /tsens.
if(CONFIG_RV9_DRV_GPIO OR CONFIG_RV9_DRV_PWM OR CONFIG_RV9_DRV_ADC OR
   CONFIG_RV9_DRV_TSENS)
    list(APPEND RV9_IO_SRCS "src/pio.c")
endif()

# The bus, for /i2c0 and for the chips audio, the camera and touch talk to.
if(CONFIG_RV9_DRV_I2C OR CONFIG_RV9_DRV_AUDIO OR CONFIG_RV9_DRV_CAMERA OR
   CONFIG_RV9_DRV_TOUCH)
    list(APPEND RV9_IO_SRCS "src/drv_i2c.c")
endif()

set(RV9_IO_REQ rv9_kal rv9_module rv9_proc driver esp_driver_i2c
               esp_driver_usb_serial_jtag esp_timer
               nvs_flash esp_driver_ledc esp_driver_gpio esp_adc
               esp_driver_tsens esp_driver_i2s)

#
# The radio.
#
# The C5 has one on the die. The P4 has none at all: its WiFi 6 comes from a
# companion ESP32-C6 over SDIO, and the host side of that link is esp_hosted
# with esp_wifi_remote on top -- see main/idf_component.yml, which is where
# the only two managed dependencies in this project are and why.
#
# What matters here is what is NOT in this file any more. nfm.c and
# drv_net.c are built for both boards, unchanged, because esp_wifi_remote
# answers esp_wifi's own API on a chip with no radio. The driver was written
# against that API and never knew where the radio was, which is the whole
# argument for an interface that does not leak.
#
# A board whose companion does not answer still has no /n0: the driver
# registers, the descriptor finds it, and the device fails to attach -- no
# driver, no device, and `have net` says no.
#
rv9_if(CONFIG_RV9_DRV_NET "src/nfm.c" "src/drv_net.c")
rv9_if(CONFIG_RV9_DRV_PEER "src/peerfm.c" "src/peerdisc.c")
# Finding peers by mDNS (main/idf_component.yml). Required always: a
# component's requirements are settled before the configuration is read, and
# a firmware that never calls it links none of it.
list(APPEND RV9_IO_REQ espressif__mdns)
list(APPEND RV9_IO_REQ lwip esp_netif esp_wifi esp_event)

#
# The camera. The driver is built everywhere and finds no camera except on
# the P4, which is the only one with the MIPI receiver, image processor and
# JPEG engine it drives -- see src/drv_camera.c.
#
rv9_if(CONFIG_RV9_DRV_CAMERA "src/drv_camera.c")

if("${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND RV9_IO_REQ esp_driver_cam esp_driver_isp esp_driver_jpeg)
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
rv9_if(CONFIG_RV9_DRV_LCDCON "src/drv_lcdcon.c")
rv9_if(CONFIG_RV9_DRV_SVGWIN "src/drv_svgwin.c" "src/raster.c")
rv9_if(CONFIG_RV9_DRV_TOUCH  "src/drv_touch.c")
if(CONFIG_RV9_DRV_LCDCON OR CONFIG_RV9_DRV_SVGWIN)
    list(APPEND RV9_IO_SRCS "src/font.c")
endif()
list(APPEND RV9_IO_REQ esp_lcd esp_hw_support)

# The panel, for whatever draws on it or touches it -- and on the boards
# whose card shares its SPI bus, for the card.
if("${IDF_TARGET}" STREQUAL "esp32p4")
    if(CONFIG_RV9_DRV_LCDCON OR CONFIG_RV9_DRV_SVGWIN OR CONFIG_RV9_DRV_TOUCH)
        list(APPEND RV9_IO_SRCS "src/panel_dsi.c")
    endif()
    rv9_if(CONFIG_RV9_DRV_SDCARD "src/drv_sdmmc.c")
    list(APPEND RV9_IO_REQ esp_driver_sdmmc sdmmc)
else()
    if(CONFIG_RV9_DRV_LCDCON OR CONFIG_RV9_DRV_SVGWIN OR CONFIG_RV9_DRV_TOUCH OR
       CONFIG_RV9_DRV_SDCARD)
        list(APPEND RV9_IO_SRCS "src/panel.c")
    endif()
    rv9_if(CONFIG_RV9_DRV_SDCARD "src/drv_sdspi.c")
    list(APPEND RV9_IO_REQ esp_driver_sdspi sdmmc)
endif()

idf_component_register(SRCS ${RV9_IO_SRCS}
                       INCLUDE_DIRS "include"
                       REQUIRES ${RV9_IO_REQ})
