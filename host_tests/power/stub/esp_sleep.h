// Host stub of ESP-IDF esp_sleep.h: the wake source numbers main/power_logic.h reads. The values are the
// first entries of esp_sleep_source_t in components/esp_hw_support/include/esp_sleep.h of IDF v6.0.2.
#pragma once

typedef enum
{
    ESP_SLEEP_WAKEUP_UNDEFINED,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
} esp_sleep_source_t;
