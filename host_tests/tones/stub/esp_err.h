// Host stub of ESP-IDF esp_err.h: the codes main/tones.cpp and components/epd97/epd97.h use.
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT       0x107

static inline const char* esp_err_to_name(esp_err_t e)
{
    switch(e)
    {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_ERR_?";
    }
}
