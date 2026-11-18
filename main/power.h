// Power of the badge: the wake cause, the system rail voltage, USB presence and deep sleep.
// The rail is measured on BAT_SENS, GPIO14 (ADC2 channel 3), through a 10k over 10k divider. The rail is
// the cell voltage on battery and VBUS less a Schottky drop on USB, so the reading gives the battery
// voltage only when USB is absent. No other signal tells the chip that USB power is present.
//
// Order at boot: power_init() once, before buttons_start() and before WiFi starts. The other functions
// need power_init() first.
// Order before sleep: stop WiFi, then power_sleep() from the task that drives the LEDs, the browser
// task once it runs.
#pragma once

#include "power_logic.h"

// Stores the wake cause, returns GPIO15 and GPIO16 from RTC function to digital function after a deep
// sleep wake, releases the LED data pad hold and sets up the ADC.
void power_init();

// The wake cause stored by power_init(). Rules in power_logic.h.
power_wake power_wake_cause();

// "power on or reset", "timer", "Menu -" or "User B".
const char* power_wake_name(power_wake w);

// One read of the system rail, the average of up to 8 samples, and the results power_judge() derives
// from it:
// - rail_mv: negative when every sample failed. ADC2 is shared with WiFi, so a read while WiFi runs
//   can lose samples.
// - usb: the rail above POWER_USB_MV, a failed read, or a connected USB host
//   (usb_serial_jtag_is_connected()). A charger with no host shows only in the rail.
// - low: false on USB. On battery true below POWER_LOW_MV, and after a true it stays true until the
//   rail reaches POWER_RESUME_MV. was_low is low from the reading before, which the caller keeps across
//   deep sleep in its RTC state. Pass false when there is none.
power_reading power_read(bool was_low);

// Sends the LEDs dark, cuts and latches off the panel supply, arms the wake on Menu - (GPIO15) and User
// B (GPIO16) and enters deep sleep. seconds > 0 also arms the timer. seconds <= 0 wakes on a button
// only. A wake button held down at the call is not armed (power_sleep_arm() in power_logic.h). A
// button only sleep waits up to 10 s for its release first. A wake restarts the firmware from app_main.
// Calls no WiFi function.
[[noreturn]] void power_sleep(int seconds);
