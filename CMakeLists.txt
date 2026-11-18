cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Builds only the components that main and its dependencies require.
# Every component main uses must then appear in PRIV_REQUIRES.
idf_build_set_property(MINIMAL_BUILD ON)
# epd97: a session close keeps the panel supply on, so the table switch between the two phases of
# every draw skips the 1.5 s off floor and the settle (EPD_KEEP_SUPPLY in
# components/epd97/epd97_config.h).
idf_build_set_property(COMPILE_DEFINITIONS "-DEPD_KEEP_SUPPLY=1" APPEND)
project(browser26)
