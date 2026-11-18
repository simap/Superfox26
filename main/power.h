// Power management for the badge.
// It reports why the device woke, measures the supply rail to tell USB power from battery and
// whether the battery is low, and enters deep sleep until a wake button or timer fires.
// Call power_init once before any other function here.
#pragma once

#include "power_logic.h"

void power_init();

power_wake power_wake_cause();

const char* power_wake_name(power_wake w);

power_reading power_read(bool was_low);

// released: buttons_released(), 0 before the button task runs (power_held_pins()).
[[noreturn]] void power_sleep(int seconds, uint64_t released = 0);
