// The badge signals its status on a strip of WS2812 LEDs.
// This module drives the strip, lighting a run of LEDs in one color to stand for the current state.
// Call leds_init once before any other function.
// The rest do nothing until it succeeds.
// The browser task sets the base bar. A tab selection, the hold mark and the red flash show over it,
// and come from the button task too, so every call takes one lock (leds_logic.h).
#pragma once

inline constexpr int LEDS_DATA_GPIO = 38;

void leds_init();
void leds_off();
void leds_red();
void leds_amber();
void leds_load(int done, int total);
void leds_timer(int remaining_ms, int total_ms);

void leds_select(int offset);
void leds_select_end();
void leds_hold(bool on);
// While busy, the selection blinks: the chosen tab waits for a load or a draw to end.
void leds_busy(bool busy);
void leds_flash();
// The blink and the end of the flash. The button task calls it on each poll.
void leds_tick();
// Dark, and nothing is written after it. A deep sleep holds the data line, and the WS2812s keep the
// last frame while they are powered.
void leds_stop();
