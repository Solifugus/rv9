# Speech to text: Whistle's engine behind /stt0.
#
# The engine is built for the P4 only. It wants PSRAM for a 17 MB model and
# the P4's vector unit to be quick enough to use, and the C5 and C3 have
# neither -- so there the driver alone is built, finds no engine, and the
# device does not attach.
#
# The engine is compiled -O2 whatever the firmware is: at -Og, which is what
# the rest of RV-9 is built with so that a fault can be read, a sentence
# takes several times as long, and the arithmetic is not where faults are.
set(STT_SRCS "src/drv_stt.c")
set(STT_INC  "include")

if("${IDF_TARGET}" STREQUAL "esp32p4")
    file(GLOB ND_SRCS "${CMAKE_CURRENT_LIST_DIR}/needle/src/*.c")
    list(APPEND STT_SRCS ${ND_SRCS})
    list(APPEND STT_INC "needle/include")
endif()

idf_component_register(SRCS ${STT_SRCS}
                       INCLUDE_DIRS ${STT_INC}
                       REQUIRES rv9_io rv9_kal rv9_module heap esp_timer)

if("${IDF_TARGET}" STREQUAL "esp32p4")
    set_source_files_properties(${ND_SRCS} PROPERTIES COMPILE_OPTIONS
        "-O2;-include;${CMAKE_CURRENT_LIST_DIR}/src/nd_rv9.h")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/needle/src/nd_tokenizer.c"
        PROPERTIES COMPILE_OPTIONS
        "-O2;-include;${CMAKE_CURRENT_LIST_DIR}/src/nd_rv9.h;-include;${CMAKE_CURRENT_LIST_DIR}/src/nd_rv9_tok.h")
endif()
