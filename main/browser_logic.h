// The power decisions of main/browser.cpp that need no hardware: the page after a wake, the sleep time
// after a page and the low battery step. host_tests/browser_power checks them.
#pragma once

#include <cstdint>
#include "power_logic.h"

// On battery, a cycle without a page (no WiFi connection, no URLs, a failed page) sleeps this long.
inline constexpr int BROWSER_RETRY_S = 300;
// A cycle waits this long for a WiFi connection.
inline constexpr int BROWSER_WIFI_WAIT_MS = 20 * 1000;
// On USB, a cycle without a page takes a new reading when the last one is older than this, so an
// unplugged badge with no WiFi, no URLs or only failing pages goes to sleep.
inline constexpr int BROWSER_USB_RECHECK_MS = 60 * 1000;
// The longest sleep after a page, 24 h. The RTC timer cannot take the full int range of page seconds.
inline constexpr int BROWSER_SLEEP_MAX_S = 24 * 3600;

// The page index for the first cycle after a start. index is the kept next page and shown the kept page
// on the panel, either one out of range when there is none. n is the list size, at least 1.
// - Reset: the next page. The index moves on before a page renders, so a page that crashed the firmware
//   is passed over. After power on the kept state is cleared and the next page is the first.
// - Timer: the next page.
// - User B: its action, one page forward from the page shown.
// - Menu -: it has no action, so the page shown loads again.
// User B and Menu - with no page shown take the next page.
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

// Seconds of sleep after a page on battery: page_s less elapsed_ms, the time the cycle took, rounded to
// the nearest second, at least 1 and at most BROWSER_SLEEP_MAX_S.
inline constexpr int browser_sleep_s(int page_s, int64_t elapsed_ms)
{
    int64_t s = (static_cast<int64_t>(page_s) * 1000 - elapsed_ms + 500) / 1000;
    if(s > BROWSER_SLEEP_MAX_S) return BROWSER_SLEEP_MAX_S;
    return s < 1 ? 1 : static_cast<int>(s);
}

enum browser_low_action
{
    BROWSER_RUN,      // not low: the normal cycle
    BROWSER_LOW_DRAW, // low and the low battery image is not on the panel: draw it, then sleep for a button
    BROWSER_LOW_SLEEP // low and the image is on the panel: sleep for a button
};

// The action and the two flags to keep in RTC memory.
struct browser_low_step
{
    browser_low_action action;
    bool               low;   // the was_low of the next power_read() call
    bool               shown; // the low battery image is on the panel
};

// low is the low of this reading (power_read()). shown is the kept flag.
inline constexpr browser_low_step browser_low_next(bool low, bool shown)
{
    if(!low) return {BROWSER_RUN, false, false};
    return {shown ? BROWSER_LOW_SLEEP : BROWSER_LOW_DRAW, true, true};
}
