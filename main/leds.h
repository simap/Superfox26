// The eight WS2812 LEDs as a bar: amber while a page loads, teal counting down to the next page.
// 10 percent brightness. Called from the browser task only.
#pragma once

void leds_init();
void leds_off();
// Amber bar: done of total LEDs lit, left to right.
void leds_load(int done, int total);
// Teal bar: ceil(remaining / total * 8) LEDs lit, so it drains to one LED before the next page.
void leds_timer(int remaining_ms, int total_ms);
