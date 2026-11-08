idf_component_register(SRCS "src/module.c" "src/crc32.c" "src/sig.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rv9_kal esp_partition
                       PRIV_REQUIRES mbedtls)
