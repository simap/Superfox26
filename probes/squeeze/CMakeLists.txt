# Probe of the squeeze kernel, main/tone_fx_squeeze.cpp, under QEMU. At boot it runs the self test
# (the kernel against its stand in), prints the result and ends with SQUEEZE PROBE DONE. run_qemu.sh
# runs it.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(squeeze_probe)
