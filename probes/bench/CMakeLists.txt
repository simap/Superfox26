# Runs the tone benchmark of the firmware (main/tones_bench.inc) without the panel, WiFi or the
# browser, for QEMU. It builds main/tones.cpp, main/tone_plan.cpp and the tone_fx kernels from the
# firmware by path with the firmware's flags, calls tones_init() and tones_bench(), and ends with the
# line BENCH DONE. Under QEMU it checks the flow only. Its cycle counts there are not timings.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
# tone_plan.cpp and tones.cpp include the headers of the panel driver.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/epd97")
project(tones_bench_probe)
