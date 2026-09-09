idf_component_register(SRCS "src/kal_freertos.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_timer heap)
