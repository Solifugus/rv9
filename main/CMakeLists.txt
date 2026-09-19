idf_component_register(SRCS "main.c" "kal_selftest.c" "kernel_test.c"
                            "module_test.c" "io_test.c" "pub_test.c" "fault_test.c" "proc_test.c" "sched_test.c" "mem_test.c" "sd_test.c"
                            "conformance.c" "ops_freertos.c" "ops_native.c" "logring.c"
                       INCLUDE_DIRS "."
                       REQUIRES rv9_kal rv9_module rv9_proc rv9_io rv9_kernel
                                rv9_ssh esp_hw_support spi_flash esp_timer)
