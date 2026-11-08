cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Builds only the components that main and its dependencies require.
# Every component main uses must then appear in PRIV_REQUIRES.
idf_build_set_property(MINIMAL_BUILD ON)
project(browser26)
