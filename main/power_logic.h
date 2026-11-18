// The decisions of main/power.cpp that need no hardware: the wake cause from the IDF wake bits, the USB
// test on the rail voltage, the low battery hysteresis, a reading from one rail read and the wake
// sources of a sleep. host_tests/power checks them.
#pragma once

#include <cstdint>
#include "esp_sleep.h"

// Why the chip started.
enum power_wake
{
    POWER_WAKE_RESET,      // power on, reset, panic restart or any other start that is not a deep sleep wake
    POWER_WAKE_TIMER,      // the deep sleep timer
    POWER_WAKE_MENU_MINUS, // SW6 Menu -, GPIO15
    POWER_WAKE_USER_B,     // SW7 User B, GPIO16
};

// The two buttons on RTC pads. Only these can wake the chip from deep sleep.
inline constexpr int POWER_PIN_MENU_MINUS = 15;
inline constexpr int POWER_PIN_USER_B     = 16;
inline constexpr uint64_t POWER_WAKE_PINS = (1ull << POWER_PIN_MENU_MINUS) | (1ull << POWER_PIN_USER_B);

// A button only sleep that has no wake pin to arm wakes on the timer after this many seconds.
inline constexpr int POWER_STUCK_WAKE_S = 3600;

// The system rail above this is USB. A full cell gives 4200 mV at most. The USB rail is VBUS less a
// Schottky drop, 4600 mV typical.
inline constexpr int POWER_USB_MV = 4350;
// On battery, the rail below this is low.
inline constexpr int POWER_LOW_MV = 3400;
// A low battery stays low until the rail reaches this.
inline constexpr int POWER_RESUME_MV = 3600;

// causes is esp_sleep_get_wakeup_causes(). ext1_pins is esp_sleep_get_ext1_wakeup_status(), and it counts
// only when causes has the ext1 bit. Rules:
// - A start that is not a deep sleep wake is RESET.
// - A button wins over the timer.
// - When both buttons are in ext1_pins, User B wins. It is the button with an action today (next tab).
// - A deep sleep wake with no known cause is TIMER, so the page cycle goes on.
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

// rail_mv is the rail in mV, negative when the read failed. A failed read counts as USB, so a fault
// never puts a badge on a bench to sleep.
inline constexpr bool power_rail_is_usb(int rail_mv)
{
    return rail_mv < 0 || rail_mv > POWER_USB_MV;
}

// The low battery state after a reading. was_low is the state the caller kept from the reading before,
// false when it has none. usb is true on USB power. A failed read is not low.
inline constexpr bool power_low_next(bool was_low, bool usb, int rail_mv)
{
    if(usb || rail_mv < 0) return false;
    return rail_mv < (was_low ? POWER_RESUME_MV : POWER_LOW_MV);
}

// One reading of the rail and what follows from it.
struct power_reading
{
    int  rail_mv; // negative when the read failed
    bool usb;
    bool low;
    bool host; // a USB host is connected, which makes usb true whatever the rail read gave
};

// host is true while a USB host is connected. rail_mv is the one rail read. was_low as for
// power_low_next(). USB and low never hold together.
inline constexpr power_reading power_judge(bool host, int rail_mv, bool was_low)
{
    bool usb = host || power_rail_is_usb(rail_mv);
    return {rail_mv, usb, power_low_next(was_low, usb, rail_mv), host};
}

// The wake sources power_sleep() arms.
struct power_arm
{
    uint64_t pins;    // ext1 wake pins, 0 for none
    int      seconds; // timer, 0 for none
};

// seconds is the argument of power_sleep(). low_pins holds the wake pins that read low just before the
// sleep. ext1 wakes on a low level, so a pin that is low would wake the chip at once and is left out.
// A button only sleep (seconds <= 0) with no pin left arms the timer for POWER_STUCK_WAKE_S, so a
// stuck button cannot keep the badge asleep for good.
inline constexpr power_arm power_sleep_arm(int seconds, uint64_t low_pins)
{
    uint64_t pins = POWER_WAKE_PINS & ~low_pins;
    if(seconds > 0) return {pins, seconds};
    if(pins) return {pins, 0};
    return {0, POWER_STUCK_WAKE_S};
}
