# litehtml fork (browsers/litehtml/fork, branch epaper) as an ESP-IDF component, built by path.
# Three files are copied into the build directory and patched (patches/xtensa.patch):
#   encodings.cpp: no exceptions in the charset prescan, encoding tables const (.data to .rodata)
#   html_tag.cpp: dynamic_cast to static_cast (no RTTI)
#   render_item.cpp: typeid only with RTTI
# Override the fork location with -DLITEHTML_FORK=<path> on the idf.py command line.

if(NOT DEFINED LITEHTML_FORK)
    set(LITEHTML_FORK "${CMAKE_CURRENT_LIST_DIR}/../../../browsers/litehtml/fork")
endif()
get_filename_component(LH "${LITEHTML_FORK}" ABSOLUTE)

set(patched_files encodings.cpp html_tag.cpp render_item.cpp)
set(pdir "${CMAKE_CURRENT_BINARY_DIR}/patched")
set(patch_file "${CMAKE_CURRENT_LIST_DIR}/patches/xtensa.patch")

# Re-run the copy and patch whenever the fork sources or the patch change.
set(patch_inputs "${patch_file}")
foreach(f ${patched_files})
    list(APPEND patch_inputs "${LH}/src/${f}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patch_inputs})

file(REMOVE_RECURSE "${pdir}")
file(MAKE_DIRECTORY "${pdir}/b/src")
foreach(f ${patched_files})
    file(COPY "${LH}/src/${f}" DESTINATION "${pdir}/b/src")
endforeach()
execute_process(COMMAND patch -p1 --quiet -i "${patch_file}"
                WORKING_DIRECTORY "${pdir}/b"
                RESULT_VARIABLE patch_rc OUTPUT_VARIABLE patch_out ERROR_VARIABLE patch_out)
if(NOT patch_rc EQUAL 0)
    message(FATAL_ERROR "litehtml: xtensa.patch did not apply to ${LH}: ${patch_out}")
endif()

execute_process(COMMAND git -C "${LH}" rev-parse --short HEAD
                OUTPUT_VARIABLE LH_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${LH}" status --porcelain --untracked-files=no
                OUTPUT_VARIABLE LH_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(LH_DIRTY)
    set(LH_REV "${LH_REV}-dirty")
endif()
message(STATUS "litehtml fork ${LH} at ${LH_REV}")

file(GLOB lh_all "${LH}/src/*.cpp")
set(lh_srcs)
foreach(s ${lh_all})
    get_filename_component(n "${s}" NAME)
    list(FIND patched_files "${n}" idx)
    if(idx EQUAL -1)
        list(APPEND lh_srcs "${s}")
    else()
        list(APPEND lh_srcs "${pdir}/b/src/${n}")
    endif()
endforeach()

set(gumbo_srcs attribute.c char_ref.c char_ref_gperf.c error.c parser.c string_buffer.c
               string_piece.c tag.c tokenizer.c utf8.c util.c vector.c)
list(TRANSFORM gumbo_srcs PREPEND "${LH}/src/gumbo/src/")

idf_component_register(SRCS ${lh_srcs} ${gumbo_srcs}
                       INCLUDE_DIRS "${LH}/include" "${LH}/include/litehtml"
                       PRIV_INCLUDE_DIRS "${LH}/src" "${LH}/src/gumbo/src")

target_compile_definitions(${COMPONENT_LIB} PUBLIC LITEHTML_REV="${LH_REV}")
# Upstream code has many warnings under IDF's -Werror=all set; keep them visible but not fatal.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-variable -Wno-unused-but-set-variable)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu99>)
