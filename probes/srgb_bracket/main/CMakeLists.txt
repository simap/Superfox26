set(fw "${CMAKE_CURRENT_LIST_DIR}/../../../main")
set(model "${CMAKE_CURRENT_LIST_DIR}/../../../host_tests/tone_fx/model")
# The bracket table comes from main/tone_tables.h, which reads headers of components/epd97.
idf_component_register(SRCS "probe_main.cpp" "${fw}/tone_fx_srgb.cpp" "${fw}/tone_fx_bracket.cpp"
                            "${model}/tone_fx_srgb_model.cpp" "${model}/tone_fx_bracket_model.cpp"
                       INCLUDE_DIRS "." "${fw}" "${model}" "${fw}/../components/epd97"
                       PRIV_REQUIRES esp_hw_support heap)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
# The kernels at -O2, as the firmware builds them. The asm blocks do not depend on it.
set_source_files_properties("${fw}/tone_fx_srgb.cpp" "${fw}/tone_fx_bracket.cpp" PROPERTIES COMPILE_OPTIONS "-O2")
# The stand ins define the kernels' names too, so this build gives them the suffix _model
# (tone_fx_model.h).
set_source_files_properties("${model}/tone_fx_srgb_model.cpp" PROPERTIES COMPILE_DEFINITIONS
                            "tone_fx_srgb_to_lin=tone_fx_srgb_to_lin_model")
set_source_files_properties("${model}/tone_fx_bracket_model.cpp" PROPERTIES COMPILE_DEFINITIONS
                            "tone_fx_bracket=tone_fx_bracket_model")
