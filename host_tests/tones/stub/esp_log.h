// Host stub of ESP-IDF esp_log.h. The log macros print to stderr.
#pragma once

#include <stdio.h>

#define ESP_LOG_HOST(lvl, tag, fmt, ...) fprintf(stderr, lvl " (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...)          ESP_LOG_HOST("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)          ESP_LOG_HOST("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)          ESP_LOG_HOST("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)          ((void)(tag))
#define ESP_LOGV(tag, fmt, ...)          ((void)(tag))
