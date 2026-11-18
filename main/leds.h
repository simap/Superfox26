// The eight WS2812 LEDs as a bar: amber while a page loads, teal counting down to the next page.
// Minimum brightness, 1 or 2 of 255 per channel. Called from the browser task only.
#pragma once

// WS2812 data, from the stock firmware's led_bar.py. power.cpp holds it low through deep sleep.
inline constexpr int LEDS_DATA_GPIO = 38;

void leds_init();
void leds_off();
// The first LED red, the others dark. The fault signal of main/main.cpp.
void leds_red();
// The first LED amber, the others dark. The browser flashes it after a failed WiFi connect.
void leds_amber();
// Amber bar: done of total LEDs lit, left to right.
void leds_load(int done, int total);
// Teal bar: ceil(remaining / total * 8) LEDs lit, so it drains to one LED before the next page.
void leds_timer(int remaining_ms, int total_ms);
// Violet bar for tab selection: the position of index among count tabs, one LED for the first tab
// up to all eight for the last.
void leds_select(int index, int count);
