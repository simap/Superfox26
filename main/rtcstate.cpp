#include "rtcstate.h"

#include <atomic>
#include <cstdint>
#include "esp_attr.h"

namespace
{
// "RTS" in the upper 3 bytes, the layout version in the low byte. Units: main/sizecache.cpp, main/wifi.c,
// main/browser.cpp.
const uint32_t RTCSTATE_MAGIC = 0x52545306;

RTC_NOINIT_ATTR uint32_t s_magic;

// The draw word. It does not depend on s_magic: only DRAW_RUNNING means a draw was cut short, and
// memory after power on holds that value by chance once in 2^32.
const uint32_t DRAW_RUNNING = 0x44524157; // "DRAW"
const uint32_t DRAW_DONE    = 0x646f6e65; // "done"

RTC_NOINIT_ATTR volatile uint32_t s_draw;
} // namespace

void rtcstate_draw_begin()
{
    s_draw = DRAW_RUNNING;
}

void rtcstate_draw_end()
{
    s_draw = DRAW_DONE;
}

bool rtcstate_draw_interrupted()
{
    return s_draw == DRAW_RUNNING;
}

bool rtcstate_begin()
{
    return s_magic == RTCSTATE_MAGIC;
}

void rtcstate_commit()
{
    // The clears of the units reach memory before the magic word.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    s_magic = RTCSTATE_MAGIC;
}
