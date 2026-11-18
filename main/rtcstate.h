// Validity of the browser state kept in RTC slow memory (RTC_NOINIT_ATTR). Deep sleep, a software reset
// and a panic restart keep that memory. After power on it holds random bits. One magic word, itself
// RTC_NOINIT_ATTR, covers the state of every unit.
//
// Rules for a unit with RTC_NOINIT_ATTR state:
// - Its init clears the state when rtcstate_begin() returned false.
// - A change of its layout changes the layout version in the magic word (main/rtcstate.cpp).
//
// Order at boot: rtcstate_begin(), the init of every unit that keeps state, then rtcstate_commit().
// begin does not write the magic word, so a reset during a clear gives another clear on the next boot.
#pragma once

// True when the magic word is right and the kept state is valid. Call once at boot.
bool rtcstate_begin();

// Writes the magic word. Call once at boot after every unit that keeps state has run its init.
void rtcstate_commit();

// The draw word, a second word in RTC memory. A panel draw that stops part way leaves the panel with a
// net DC voltage, and repeated cut draws can damage it. rtcstate_draw_begin() marks a draw as running
// and rtcstate_draw_end() marks it as complete. rtcstate_draw_interrupted() is true at boot when a
// reset, a brownout or a crash cut the last draw. The firmware must then not draw again (main/main.cpp).
// Removing power from the badge clears the word.
void rtcstate_draw_begin();
void rtcstate_draw_end();
bool rtcstate_draw_interrupted();
