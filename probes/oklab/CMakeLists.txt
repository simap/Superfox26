# Probe of the Oklab fixed point kernels (main/tone_fx_oklab.cpp) under QEMU. It builds the kernel
# file from ../../main by path beside its stand ins, prints the vector instruction checks, runs the
# self test at boot and prints the mismatch count, then OKLAB PROBE DONE. run_qemu.sh runs it.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(oklab_probe)
