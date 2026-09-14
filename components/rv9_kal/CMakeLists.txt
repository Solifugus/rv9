# One backend or the other, never both. Everything above the KAL is
# identical either way -- that is the entire point of the seam.
#
# The requirements are listed unconditionally even though only one backend
# uses each: ESP-IDF resolves component dependencies in an early pass,
# before Kconfig is loaded, so a CONFIG_-dependent REQUIRES silently
# resolves to whichever branch happens to be false at that moment.
if(CONFIG_RV9_KERNEL_NATIVE)
    set(kal_backend "src/kal_native.c")
else()
    set(kal_backend "src/kal_freertos.c")
endif()

# kal_rt.c is built either way: real-time tasks are hosted by the host's
# preemptive scheduler regardless of which kernel runs everything else,
# because RV-9's own is cooperative and cannot bound latency.
idf_component_register(SRCS ${kal_backend} "src/kal_rt.c" "src/kal_mem.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_kernel freertos esp_timer esp_system heap)
