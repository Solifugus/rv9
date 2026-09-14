idf_component_register(SRCS "src/io.c" "src/claim.c" "src/scf.c" "src/drv_uart.c"
                            "src/drv_lcdcon.c" "src/font.c"
                            "src/rbf.c" "src/drv_ramdisk.c"
                            "src/nfm.c" "src/drv_net.c" "src/drv_flashdisk.c"
                            "src/pio.c" "src/drv_gpio.c" "src/console.c" "src/drv_pwm.c"
                            "src/panel.c" "src/raster.c" "src/drv_svgwin.c"
                            "src/drv_adc.c" "src/drv_tsens.c"
                            "src/pfm.c" "src/drv_pubmem.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_kal rv9_module rv9_proc esp_lcd driver esp_driver_usb_serial_jtag lwip esp_netif esp_wifi esp_event esp_timer nvs_flash
                                esp_driver_ledc esp_driver_gpio esp_adc esp_driver_tsens)
