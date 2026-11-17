# Probe of the tone_fx srgb and bracket kernels under QEMU. Builds main/tone_fx_srgb.cpp and
# main/tone_fx_bracket.cpp of the firmware by path beside their stand ins, runs the instruction check
# and the self tests at boot and prints PROBE DONE last. See main/probe_main.cpp. run_qemu.sh runs it.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(tone_fx_srgb_bracket_probe)
