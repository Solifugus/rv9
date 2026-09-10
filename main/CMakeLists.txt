idf_component_register(SRCS "main.c" "kal_selftest.c" "kernel_test.c"
                       INCLUDE_DIRS "."
                       REQUIRES rv9_kal rv9_module rv9_proc rv9_io rv9_kernel
                                esp_hw_support spi_flash)
