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

idf_component_register(SRCS ${kal_backend}
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_kernel freertos esp_timer heap)
