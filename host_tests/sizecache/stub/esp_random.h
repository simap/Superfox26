// Host stub of ESP-IDF esp_random.h. sizecache_test.cpp defines it as a seeded xorshift, so a run
// repeats.
#pragma once

#include <stdint.h>

uint32_t esp_random(void);
