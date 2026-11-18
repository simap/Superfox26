// What the LED bar shows, as pure functions of the layers it has to show. leds.cpp writes the result
// to the strip. host_tests/leds checks these functions.
#pragma once

#include <cstdint>

inline constexpr int LEDS_N = 8;
// A selection that waits for a load or a draw blinks: this long on, this long off.
inline constexpr int64_t LEDS_BLINK_US = 500 * 1000;

enum leds_color : uint8_t
{
    LEDS_DARK,
    LEDS_AMBER,
    LEDS_TEAL,
    LEDS_VIOLET,
    LEDS_WHITE,
    LEDS_BLUE,
    LEDS_RED,
    LEDS_GREEN,
};

struct leds_frame
{
    leds_color px[LEDS_N];
};

inline constexpr bool leds_same(const leds_frame& a, const leds_frame& b)
{
    for(int i = 0; i < LEDS_N; i++)
        if(a.px[i] != b.px[i]) return false;
    return true;
}

// LEDs lit for done of total, rounded up, so a countdown keeps one LED until it ends.
inline constexpr int leds_ceil_lit(int64_t done, int64_t total)
{
    if(total <= 0 || done <= 0) return 0;
    int64_t lit = (done * LEDS_N + total - 1) / total;
    return lit > LEDS_N ? LEDS_N : static_cast<int>(lit);
}

// A tab selection as its offset from the page on the panel. Forward runs from the left end, back from
// the right end, one LED per tab. A run stops at 7 LEDs, so a forward run and a back run never light the
// same LEDs. Past 7 tabs the run starts again in the next colour, and past 28 it stays at 7 green.
// Offset 0, the page on the panel, lights the two middle LEDs, which no run does alone.
inline constexpr int LEDS_LAP = LEDS_N - 1;

inline constexpr leds_frame leds_offset_frame(int offset)
{
    leds_frame f{};
    if(offset == 0)
    {
        f.px[LEDS_N / 2 - 1] = f.px[LEDS_N / 2] = LEDS_VIOLET;
        return f;
    }
    const leds_color laps[] = {LEDS_VIOLET, LEDS_WHITE, LEDS_BLUE, LEDS_GREEN};
    const int        nlaps  = sizeof laps / sizeof laps[0];
    int              k      = offset < 0 ? -offset : offset;
    int              lap    = (k - 1) / LEDS_LAP;
    int              lit    = (k - 1) % LEDS_LAP + 1;
    if(lap >= nlaps)
    {
        lap = nlaps - 1;
        lit = LEDS_LAP;
    }
    for(int i = 0; i < lit; i++) f.px[offset > 0 ? i : LEDS_N - 1 - i] = laps[lap];
    return f;
}

// since_us is the time from the last press of the selection, so a press always shows at once.
inline constexpr bool leds_blink_off(int64_t since_us)
{
    return since_us >= 0 && (since_us / LEDS_BLINK_US) % 2 == 1;
}

struct leds_layers
{
    leds_color base_color = LEDS_DARK; // the bar of the browser task: load, countdown or off
    int        base_lit   = 0;
    bool       base_timer = false; // the base is the countdown bar
    bool       hold       = false;
    bool       select     = false;
    int        offset     = 0;
    bool       select_off = false; // the selection is in the off half of its blink
    bool       flash      = false; // a press with no page list
};

// A held countdown shows full and does not drain. Any other held bar shows hold on the last LED.
inline constexpr leds_frame leds_compose(const leds_layers& l)
{
    leds_frame f{};
    if(l.flash)
    {
        f.px[0] = LEDS_RED;
        return f;
    }
    if(l.select && !l.select_off) return leds_offset_frame(l.offset);
    int lit = l.hold && l.base_timer ? LEDS_N : l.base_lit;
    if(lit > LEDS_N) lit = LEDS_N;
    for(int i = 0; i < lit; i++) f.px[i] = l.base_color;
    if(l.hold && !l.base_timer) f.px[LEDS_N - 1] = LEDS_TEAL;
    return f;
}
