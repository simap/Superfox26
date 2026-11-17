# The kernel builds from the firmware's main directory by path, with the flags tones.cpp gets
# there (-O2, no exceptions, no RTTI). Its stand in builds from host_tests/tone_fx/model. Its
# parameters come from main/tone_tables.h, which reads headers of components/epd97.
set(fw_main "${CMAKE_CURRENT_LIST_DIR}/../../../main")
set(model "${CMAKE_CURRENT_LIST_DIR}/../../../host_tests/tone_fx/model")
idf_component_register(SRCS "squeeze_probe.cpp" "${fw_main}/tone_fx_squeeze.cpp" "${model}/tone_fx_squeeze_model.cpp"
                       INCLUDE_DIRS "." "${fw_main}" "${model}" "${fw_main}/../components/epd97"
                       PRIV_REQUIRES esp_hw_support heap)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
set_source_files_properties("${fw_main}/tone_fx_squeeze.cpp" PROPERTIES COMPILE_OPTIONS "-O2")
# The stand in defines the kernel's name too, so this build gives it the suffix _model
# (tone_fx_model.h).
set_source_files_properties("${model}/tone_fx_squeeze_model.cpp" PROPERTIES COMPILE_DEFINITIONS
                            "tone_fx_squeeze=tone_fx_squeeze_model")
