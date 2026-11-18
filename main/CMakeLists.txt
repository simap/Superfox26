# The fixed point tone kernels (tone_fx.h), which the dither of tones.cpp runs on.
set(tone_fx_srcs "tone_fx_srgb.cpp" "tone_fx_bracket.cpp" "tone_fx_squeeze.cpp" "tone_fx_search.cpp"
                 "tone_fx_oklab.cpp")

# MINIMAL_BUILD is on, so every component main uses is listed here.
idf_component_register(SRCS "main.cpp" "browser.cpp" "console.cpp" "config.cpp" "wifi.c" "fetch.cpp"
                            "url.cpp" "fonts.cpp" "image.cpp" "render.cpp" "leds.cpp" "buttons.cpp" "sizecache.cpp"
                            "rtcstate.cpp" "tabs.cpp" "tones.cpp" "tone_plan.cpp" "power.cpp"
                            ${tone_fx_srcs}
                            "image_gif_bmp.cpp"
                            "image_webp.cpp"
                       INCLUDE_DIRS "."
                       EMBED_FILES "../logo/panel_32.png" "../logo/panel_200.png"
                       PRIV_REQUIRES litehtml epd97 esp_psram esp_partition esp_timer heap esp_app_format
                                     nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls
                                     esp_driver_usb_serial_jtag esp_driver_gpio esp_adc vfs joltwallet__littlefs
                                     espressif__led_strip
                                     espressif__freetype espressif__libpng espressif__libjpeg-turbo
                                     webp)

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
# The dither is per pixel code over the whole panel. At the project's -Os its helpers stay out of
# line and the row loop spills registers around every call. The tone benchmark (probes/bench)
# builds these files with the same flags.
set_source_files_properties(tones.cpp ${tone_fx_srcs} PROPERTIES COMPILE_OPTIONS "-O2")

# The data partition image: a LittleFS volume built from build/fsroot, which holds the font pack
# tools/stage_fonts.py copies from data/fonts (the output of tools/fontpack). B26_FONT_DIR in the
# environment at configure time picks another pack. urls.txt is not in the image: the console
# writes it on the device, and `idf.py app-flash` leaves the partition alone. `idf.py flash`
# rewrites it.
idf_build_get_property(python PYTHON)
set(fsroot "${CMAKE_BINARY_DIR}/fsroot")
set(stage_fonts "${CMAKE_CURRENT_LIST_DIR}/../tools/stage_fonts.py")
if(DEFINED ENV{B26_FONT_DIR})
    set(font_dir "$ENV{B26_FONT_DIR}")
else()
    set(font_dir "${CMAKE_CURRENT_LIST_DIR}/../data/fonts")
endif()
file(GLOB font_files CONFIGURE_DEPENDS "${font_dir}/*")
set(fonts_stamp "${CMAKE_BINARY_DIR}/fonts_staged.stamp")
add_custom_command(OUTPUT "${fonts_stamp}"
                   COMMAND ${python} "${stage_fonts}" "${fsroot}" "${font_dir}"
                   COMMAND ${CMAKE_COMMAND} -E touch "${fonts_stamp}"
                   DEPENDS "${stage_fonts}" ${font_files}
                   COMMENT "staging fonts for the data partition")
add_custom_target(stage_fonts DEPENDS "${fonts_stamp}")
littlefs_create_partition_image(data "${fsroot}" FLASH_IN_PROJECT DEPENDS stage_fonts)
