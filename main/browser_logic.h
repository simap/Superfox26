// Decision logic for the badge browser, held apart from the input and output in browser.cpp.
// These functions choose the page index to show for each wake cause, the seconds to sleep before
// the next page, and the action to take when the battery is low.
// Each one computes its result from its arguments alone and keeps no state, so the browser's policy
// can be tested without the device.
#pragma once

#include <cstdint>
#include "power_logic.h"

inline constexpr int BROWSER_RETRY_S = 300;
inline constexpr int BROWSER_WIFI_WAIT_MS = 20 * 1000;
inline constexpr int BROWSER_USB_RECHECK_MS = 60 * 1000;
inline constexpr int BROWSER_SLEEP_MAX_S = 24 * 3600;

inline constexpr int browser_wake_index(power_wake wake, int index, int shown, int n)
{
    bool have_index = index >= 0 && index < n;
    bool have_shown = shown >= 0 && shown < n;
    int  next       = have_index ? index : 0;
    switch(wake)
    {
    case POWER_WAKE_RESET:
    case POWER_WAKE_TIMER: return next;
    case POWER_WAKE_USER_B: return have_shown ? (shown + 1) % n : next;
    case POWER_WAKE_MENU_MINUS: return have_shown ? shown : next;
    }
    return next;
}

inline constexpr int browser_sleep_s(int page_s, int64_t elapsed_ms)
{
    int64_t s = (static_cast<int64_t>(page_s) * 1000 - elapsed_ms + 500) / 1000;
    if(s > BROWSER_SLEEP_MAX_S) return BROWSER_SLEEP_MAX_S;
    return s < 1 ? 1 : static_cast<int>(s);
}

inline constexpr int browser_wrap(int i, int n)
{
    return n > 0 ? (i % n + n) % n : 0;
}

// The offset of target from base the short way round a list of n, from -(n - 1) / 2 to n / 2. The LED
// bar shows it (leds_logic.h).
inline constexpr int browser_offset(int target, int base, int n)
{
    int d = browser_wrap(target - base, n);
    return d > n / 2 ? d - n : d;
}

enum browser_sel_action
{
    BROWSER_SEL_NONE, // no selection
    BROWSER_SEL_WAIT, // its hold runs: more presses can come
    BROWSER_SEL_DROP, // it changes nothing, so it closes and the cycle goes on as before the press
    BROWSER_SEL_LOAD, // its page loads, and a render of another page is abandoned
};

// A tab selection after its hold. base is the page its offset counts from. loading is the page being
// rendered or about to be drawn, -1 for none. A forced selection (a wake, the console) always loads.
inline constexpr browser_sel_action browser_sel_next(bool open, bool due, bool force, int target, int base,
                                                     int loading)
{
    if(!open) return BROWSER_SEL_NONE;
    if(!due) return BROWSER_SEL_WAIT;
    if(!force && (target == base || target == loading)) return BROWSER_SEL_DROP;
    return BROWSER_SEL_LOAD;
}

enum browser_low_action
{
    BROWSER_RUN,
    BROWSER_LOW_DRAW,
    BROWSER_LOW_SLEEP
};

struct browser_low_step
{
    browser_low_action action;
    bool               low;
    bool               shown;
};

inline constexpr browser_low_step browser_low_next(bool low, bool shown)
{
    if(!low) return {BROWSER_RUN, false, false};
    return {shown ? BROWSER_LOW_SLEEP : BROWSER_LOW_DRAW, true, true};
}
