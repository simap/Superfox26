set(fw "${CMAKE_CURRENT_LIST_DIR}/../../../main")
set(model "${CMAKE_CURRENT_LIST_DIR}/../../../host_tests/tone_fx/model")
# The kernel's table comes from main/tone_tables.h, which reads headers of components/epd97.
idf_component_register(SRCS "probe_main.cpp" "${fw}/tone_fx_search.cpp" "${model}/tone_fx_search_model.cpp"
                       INCLUDE_DIRS "." "${fw}" "${model}" "${fw}/../components/epd97")

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti> -O2)
# The stand in defines the kernel's name too, so this build gives it the suffix _model
# (tone_fx_model.h).
set_source_files_properties("${model}/tone_fx_search_model.cpp" PROPERTIES COMPILE_DEFINITIONS
                            "tone_fx_search=tone_fx_search_model")
