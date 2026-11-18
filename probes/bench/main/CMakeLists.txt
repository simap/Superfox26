# The tone sources of the firmware by path. The kernel list and the flags match
# main/CMakeLists.txt: -Os for the project, -O2 for tones.cpp and the kernels, TONES_BENCH on. Keep
# tone_fx_srcs in step with the list of that name there.
set(fw "${CMAKE_CURRENT_LIST_DIR}/../../../main")
set(tone_fx_srcs "${fw}/tone_fx_srgb.cpp" "${fw}/tone_fx_bracket.cpp" "${fw}/tone_fx_squeeze.cpp"
                 "${fw}/tone_fx_search.cpp" "${fw}/tone_fx_oklab.cpp")
idf_component_register(SRCS "probe_main.cpp" "${fw}/tones.cpp" "${fw}/tone_plan.cpp" ${tone_fx_srcs}
                       INCLUDE_DIRS "." "${fw}"
                       PRIV_REQUIRES epd97 esp_psram esp_timer esp_hw_support heap freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
set_source_files_properties("${fw}/tones.cpp" ${tone_fx_srcs} PROPERTIES COMPILE_OPTIONS "-O2")
# TONES_BENCH is on unless the build sets -DTONES_BENCH=0. In its own build directory,
# $IDF_PY -B build_nobench -DTONES_BENCH=0 build, the tone code builds as the firmware has it, without
# the benchmark and the float dither, and the probe runs tones_init() alone.
if(NOT DEFINED TONES_BENCH OR TONES_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TONES_BENCH=1)
    # -DTONES_BENCH_ALL=1 adds the kernel rows and the float frames without the cache.
    if(TONES_BENCH_ALL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE TONES_BENCH_ALL=1)
    endif()
endif()
