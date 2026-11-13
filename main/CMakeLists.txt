set(srcs "main.c" "kal_selftest.c" "logring.c" "clock.c" "health.c")

# The suites a boot runs only when /f0/selftest asks; see main.c.
if(CONFIG_RV9_SELFTEST)
    list(APPEND srcs "kernel_test.c" "module_test.c" "io_test.c" "pub_test.c"
                     "fault_test.c" "proc_test.c" "sched_test.c" "mem_test.c"
                     "sd_test.c" "conformance.c" "ops_freertos.c" "ops_native.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES rv9_kal rv9_module rv9_proc rv9_io rv9_kernel
                                rv9_ssh rv9_tts esp_hw_support spi_flash esp_timer
                                nvs_flash esp_system riscv)

# What a panic says, and where, kept across the reset it causes: logring.c
# wraps ESP-IDF's panic handler (design section 82).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
