// The page cycle: a task on core 1 that renders the next page at its scroll offset, refreshes
// the panel, and holds it for the page's seconds (CONFIG_B26_CYCLE_SECONDS when unset). On USB it
// waits awake and counts down on the LEDs. On battery it deep sleeps (power.h). Waits for WiFi and a
// non-empty list when either is missing, and on battery sleeps BROWSER_RETRY_S (browser_logic.h).
#pragma once

#include "power.h"

// Clears the page index, the panel content and the low battery flag kept in RTC memory when kept, the result of
// rtcstate_begin(), is false. Then reads the rail for the first cycle and returns the reading. Call
// after power_init() and before wifi_init(): the read needs the radio off. With low set the first
// cycle draws the low battery notice or sleeps at once, and needs no radio.
power_reading browser_init(bool kept);

// Starts the task. browser_init, epd_begin, render_init, cfg_init and wifi_init must have run.
bool browser_start();

// Ends the current wait and starts the next page.
void browser_next();

// Ends the current wait and starts the page before the one shown.
void browser_prev();

// Ends the current wait and starts the page at index, from 0. An index past the list wraps. Call in
// the "wait" state only.
void browser_load(int index);

// The built-in notices.
enum browser_screen
{
    BROWSER_SCREEN_LOWBATT = 1, // the low battery notice
    BROWSER_SCREEN_WELCOME = 2, // the welcome notice, drawn when no WiFi network is stored
};

// Ends the current wait and draws the notice instead of the next page, when it is not on the panel.
// The notice holds for CONFIG_B26_CYCLE_SECONDS, then the cycle goes on. browser_next() ends the hold
// early. Call in the "wait", "nowifi" and "nourls" states only. In "nowifi" with an SSID stored the
// draw starts when the connect wait ends, up to 20 s later.
void browser_show(browser_screen screen);

// True when the notice is on the panel, from the panel content kept in RTC memory.
bool browser_shows(browser_screen screen);

// Button step during the countdown: moves a tab selection by delta (+1 or -1, wrapping) starting
// from the page shown, shows it on the LEDs, and loads it once no step has come for SELECT_HOLD_MS.
// Ignored outside the countdown.
void browser_step(int delta);

// Index into the URL list of the page being shown or rendered.
int browser_page_index();

// One word: "wait", "nowifi", "nourls", "render", "refresh", "lowbatt", "sleep".
const char* browser_state();
