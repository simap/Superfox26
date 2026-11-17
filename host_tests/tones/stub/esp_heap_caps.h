// Host stub of ESP-IDF esp_heap_caps.h. Every capability is plain malloc.
#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT     (1u << 2)

static inline void* heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void* p)
{
    free(p);
}
