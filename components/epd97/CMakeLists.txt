# Brownout hold-off writes RTCCNTL through soc/rtc_cntl_struct.h, and soc is a
# common requirement. gpio_hold_en and gpio_deep_sleep_hold_en are in
# esp_driver_gpio. soc/spi_periph.h is in esp_hal_gpspi on IDF 6.
idf_component_register(SRCS "epd97.c" "epd97_gfx.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_spi esp_timer
                       PRIV_REQUIRES esp_hal_gpspi)
