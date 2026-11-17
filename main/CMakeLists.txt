# MINIMAL_BUILD is on, so every component main uses is listed here.
idf_component_register(SRCS "main.cpp" "browser.cpp" "console.cpp" "config.cpp" "wifi.c" "fetch.cpp"
                            "url.cpp" "fonts.cpp" "image.cpp" "render.cpp" "leds.cpp" "buttons.cpp" "sizecache.cpp"
                            "tabs.cpp" "tones.cpp"
                            "image_legacy.cpp"
                            "image_webp.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES litehtml epd97 esp_psram esp_partition esp_timer heap
                                     nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls
                                     esp_driver_usb_serial_jtag esp_driver_gpio vfs joltwallet__littlefs
                                     espressif__led_strip
                                     espressif__freetype espressif__libpng espressif__libjpeg-turbo
                                     webp)

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
# The dither runs per pixel over the whole panel. At the project's -Os its helpers stay out of line
# and the row loop spills around every call.
set_source_files_properties(tones.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# The data partition image: a LittleFS volume built from build/fsroot, which holds the three
# TTF faces tools/stage_fonts.py copies from the host (B26_FONT_DIR and B26_FONT_* in the
# environment pick other files). urls.txt is not in the image: the console writes it on the
# device, and `idf.py app-flash` leaves the partition alone. `idf.py flash` rewrites it.
idf_build_get_property(python PYTHON)
set(fsroot "${CMAKE_BINARY_DIR}/fsroot")
set(stage_fonts "${CMAKE_CURRENT_LIST_DIR}/../tools/stage_fonts.py")
set(staged_fonts "${fsroot}/fonts/sans.ttf" "${fsroot}/fonts/bold.ttf" "${fsroot}/fonts/mono.ttf"
                 "${fsroot}/fonts/monobold.ttf")
add_custom_command(OUTPUT ${staged_fonts}
                   COMMAND ${python} "${stage_fonts}" "${fsroot}"
                   DEPENDS "${stage_fonts}"
                   COMMENT "staging fonts for the data partition")
add_custom_target(stage_fonts DEPENDS ${staged_fonts})
littlefs_create_partition_image(data "${fsroot}" FLASH_IN_PROJECT DEPENDS stage_fonts)
