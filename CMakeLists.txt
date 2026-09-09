cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rv9)

# The layering check is not optional and not a lint step you can skip. It is
# the mechanism that keeps phase 7 (native kernel) from becoming a rewrite.
# See docs/design.md §4.
add_custom_target(rv9_layering_check ALL
    COMMAND "${CMAKE_SOURCE_DIR}/tools/check_layering.sh" "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking RV-9 layering (no FreeRTOS above the KAL)"
    VERBATIM)

add_dependencies(${CMAKE_PROJECT_NAME}.elf rv9_layering_check)
