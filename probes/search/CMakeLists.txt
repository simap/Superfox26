# Probe of the search kernel (main/tone_fx_search.cpp) under QEMU. It builds the kernel file from
# ../../main by path beside its stand in, checks the vector instructions the kernel relies on, runs
# the self test at boot and prints the mismatch count, then SEARCH PROBE DONE. run_qemu.sh runs it.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(search_probe)
