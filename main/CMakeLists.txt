idf_component_register(SRCS "main.c" "kal_selftest.c"
                       INCLUDE_DIRS "."
                       REQUIRES rv9_kal rv9_module rv9_proc esp_hw_support spi_flash)
