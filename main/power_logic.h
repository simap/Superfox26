// Power policy for the badge: decode which event woke it, judge whether it runs on USB or a low
// battery, and build the wake sources to arm before deep sleep.
// The functions are pure and constexpr so the policy stays in one place and holds no state of its
// own.
// A rail_mv below zero means the battery reading is unavailable, and the code treats it as running
// on USB and never low.
// The low state is sticky: the caller passes the last low result back as was_low, and after the
// rail falls below POWER_LOW_MV it must climb back to POWER_RESUME_MV to clear low.
#pragma once

#include <cstdint>
#include "esp_sleep.h"

enum power_wake
{
    POWER_WAKE_RESET,
    POWER_WAKE_TIMER,
    POWER_WAKE_MENU_MINUS,
    POWER_WAKE_USER_B,
};

inline constexpr int POWER_PIN_MENU_MINUS = 15;
inline constexpr int POWER_PIN_USER_B     = 16;
inline constexpr uint64_t POWER_WAKE_PINS = (1ull << POWER_PIN_MENU_MINUS) | (1ull << POWER_PIN_USER_B);

inline constexpr int POWER_STUCK_WAKE_S = 3600;

inline constexpr int POWER_USB_MV = 4350;
inline constexpr int POWER_LOW_MV = 3400;
inline constexpr int POWER_RESUME_MV = 3600;

inline constexpr power_wake power_decode_wake(uint32_t causes, uint64_t ext1_pins)
{
    if(causes & (1u << ESP_SLEEP_WAKEUP_UNDEFINED)) return POWER_WAKE_RESET;
    if(causes & (1u << ESP_SLEEP_WAKEUP_EXT1))
    {
        if(ext1_pins & (1ull << POWER_PIN_USER_B)) return POWER_WAKE_USER_B;
        if(ext1_pins & (1ull << POWER_PIN_MENU_MINUS)) return POWER_WAKE_MENU_MINUS;
    }
    return POWER_WAKE_TIMER;
}

inline constexpr bool power_rail_is_usb(int rail_mv)
{
    return rail_mv < 0 || rail_mv > POWER_USB_MV;
}

inline constexpr bool power_low_next(bool was_low, bool usb, int rail_mv)
{
    if(usb || rail_mv < 0) return false;
    return rail_mv < (was_low ? POWER_RESUME_MV : POWER_LOW_MV);
}

struct power_reading
{
    int  rail_mv;
    bool usb;
    bool low;
    bool host;
};

inline constexpr power_reading power_judge(bool host, int rail_mv, bool was_low)
{
    bool usb = host || power_rail_is_usb(rail_mv);
    return {rail_mv, usb, power_low_next(was_low, usb, rail_mv), host};
}

struct power_arm
{
    uint64_t pins;
    int      seconds;
};

// The wake pins a sleep waits on and leaves out while still low. low is the pins low at the sleep.
// released is the pins the button task has seen high since it started: one of those that is low is a
// press made while the badge was awake, so it stays armed and wakes the chip at once with its own wake
// cause. A pin low since the start is a button held through the boot or a stuck one.
inline constexpr uint64_t power_held_pins(uint64_t low, uint64_t released)
{
    return low & ~released & POWER_WAKE_PINS;
}

inline constexpr power_arm power_sleep_arm(int seconds, uint64_t low_pins)
{
    uint64_t pins = POWER_WAKE_PINS & ~low_pins;
    if(seconds > 0) return {pins, seconds};
    if(pins) return {pins, 0};
    return {0, POWER_STUCK_WAKE_S};
}
