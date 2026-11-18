// Reads the badge buttons and turns a press into browser tab navigation.
// User A steps back one tab and User B steps forward one tab.
// Menu + and Menu - both hold the page, and a second press of either resumes the cycle.
#pragma once

#include <cstdint>

void buttons_start();

// The pins, as a mask, of the buttons that have read high since buttons_start (power_held_pins()).
uint64_t buttons_released();
