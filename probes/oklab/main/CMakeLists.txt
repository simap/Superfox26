set(fw "${CMAKE_CURRENT_LIST_DIR}/../../../main")
set(model "${CMAKE_CURRENT_LIST_DIR}/../../../host_tests/tone_fx/model")
idf_component_register(SRCS "probe_main.cpp" "${fw}/tone_fx_oklab.cpp" "${model}/tone_fx_oklab_model.cpp"
                       INCLUDE_DIRS "." "${fw}" "${model}")

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti> -O2)
# The stand ins define the kernels' names too, so this build gives them the suffix _model
# (tone_fx_model.h).
set_source_files_properties("${model}/tone_fx_oklab_model.cpp" PROPERTIES COMPILE_DEFINITIONS
                            "tone_fx_cbrt=tone_fx_cbrt_model;tone_fx_lin_to_oklab=tone_fx_lin_to_oklab_model;tone_fx_oklab_to_lin=tone_fx_oklab_to_lin_model;tone_fx_lin_to_oklab_1=tone_fx_lin_to_oklab_1_model")
