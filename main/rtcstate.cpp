// Keeps a small amount of state that survives a reset or deep sleep, so a wake can tell a first
// boot from a later one and can tell whether a draw was cut short by a reset.
// rtcstate_commit stores the magic last, behind a full memory fence, so rtcstate_begin reports the
// state as valid only after every earlier write to it is in place.

#include "rtcstate.h"

#include <atomic>
#include <cstdint>
#include "esp_attr.h"

namespace
{
const uint32_t RTCSTATE_MAGIC = 0x52545308;

RTC_NOINIT_ATTR uint32_t s_magic;

const uint32_t DRAW_RUNNING = 0x44524157;
const uint32_t DRAW_DONE    = 0x646f6e65;

RTC_NOINIT_ATTR volatile uint32_t s_draw;
}

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
    std::atomic_thread_fence(std::memory_order_seq_cst);
    s_magic = RTCSTATE_MAGIC;
}
