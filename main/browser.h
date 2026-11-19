// The browser cycles through the configured URLs, renders each page on the e-paper display, and
// holds it until a timer runs out or a button moves to another page.
// It also puts the low-battery and welcome notices on the panel.
// Call browser_init before browser_start.
#pragma once

#include "power.h"

// The low state the last boot left, false when kept is false.
bool browser_was_low(bool kept);

// low true starts the browser in the low state, as a low reading would.
power_reading browser_init(bool kept, bool low);

bool browser_start();

void browser_next();

void browser_prev();

void browser_load(int index);

enum browser_screen
{
    BROWSER_SCREEN_LOWBATT = 1,
    BROWSER_SCREEN_WELCOME = 2,
};

void browser_show(browser_screen screen);

bool browser_shows(browser_screen screen);

// A press of User A (-1) or User B (+1). Returns the chosen page index, -1 when there is no page list.
int browser_step(int delta);

// A press of Menu + or Menu -. Returns 1 when the page now holds, 0 when the cycle resumes, and -1
// when the badge is past the point of going to sleep.
int browser_hold_toggle();

int browser_page_index();

const char* browser_state();
