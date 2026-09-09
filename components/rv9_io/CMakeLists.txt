idf_component_register(SRCS "src/io.c" "src/scf.c" "src/drv_uart.c"
                            "src/drv_lcdcon.c" "src/font.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_kal rv9_module rv9_proc esp_lcd driver)
