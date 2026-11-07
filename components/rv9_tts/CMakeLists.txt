# Text to speech: CMU Flite behind /tts0.
#
# The engine is built for the P4 only, which has the external memory for a
# voice and a floating-point unit to run it; on the C5 and C3 the driver
# alone is built, finds no room, and the device does not attach.
#
# flite/ is upstream, unchanged (see flite.from). Two files are RV-9's
# modified copies, in src/, and the originals are left out: the vocoder in
# single precision (flite_mlsa.c), and opening a voice RV-9 has already
# read into memory (flite_file.c). The engine is compiled -O2, as Whistle's
# is, whatever the firmware is.
set(TTS_SRCS "src/drv_tts.c")
set(TTS_INC  "include")

if("${IDF_TARGET}" STREQUAL "esp32p4")
    set(F "${CMAKE_CURRENT_LIST_DIR}/flite")
    set(FL_SRCS "")
    foreach(d utils regex hrg stats speech lexicon synth cg wavesynth)
        file(GLOB s "${F}/src/${d}/*.c")
        list(APPEND FL_SRCS ${s})
    endforeach()
    list(FILTER FL_SRCS EXCLUDE REGEX
         "cst_mmap_(posix|win32)|cst_file_(stdio|wince|palmos)|cst_socket|cst_mlsa\\.c")
    file(GLOB lang "${F}/lang/usenglish/*.c" "${F}/lang/cmulex/*.c")
    # Four of the lexicon's files are #included by another, not compiled.
    list(FILTER lang EXCLUDE REGEX
         "cmu_lex_(data_raw|entries_huff_table|num_bytes|phones_huff_table)\\.c")
    list(APPEND FL_SRCS ${lang}
         "${F}/src/audio/audio.c" "${F}/src/audio/au_streaming.c"
         "${F}/src/audio/au_none.c"
         "src/flite_mlsa.c" "src/flite_file.c")
    list(APPEND TTS_SRCS ${FL_SRCS})
    list(APPEND TTS_INC "src" "${F}/include")
endif()

idf_component_register(SRCS ${TTS_SRCS}
                       INCLUDE_DIRS ${TTS_INC}
                       REQUIRES rv9_io rv9_kal rv9_module heap esp_timer)

if("${IDF_TARGET}" STREQUAL "esp32p4")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${F}/src/cg" "${F}/lang/usenglish" "${F}/lang/cmulex")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CST_NO_SOCKETS CST_AUDIO_NONE CST_USER_MALLOC)
    # Upstream's warnings are upstream's.
    set_source_files_properties(${FL_SRCS} PROPERTIES COMPILE_OPTIONS "-O2;-w")
    set_source_files_properties("src/flite_mlsa.c" PROPERTIES COMPILE_OPTIONS
        "-O2;-w;-fsingle-precision-constant")
endif()
