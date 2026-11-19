// Host test of main/power_logic.h: the wake cause decode, the USB threshold, the low and critical
// battery hysteresis, a reading from one rail read, the early panel supply on a button wake and the
// wake sources of a sleep. Usage: power_test. Exit status 0 when every check passes.

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "power_logic.h"

namespace
{
unsigned s_fails = 0;

void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if(!ok) s_fails++;
}

const uint32_t RESET = 1u << ESP_SLEEP_WAKEUP_UNDEFINED;
const uint32_t TIMER = 1u << ESP_SLEEP_WAKEUP_TIMER;
const uint32_t EXT1  = 1u << ESP_SLEEP_WAKEUP_EXT1;
const uint32_t EXT0  = 1u << ESP_SLEEP_WAKEUP_EXT0;

const uint64_t P15   = 1ull << 15;
const uint64_t P16   = 1ull << 16;
const uint64_t P3    = 1ull << 3;

const char* wake_name(power_wake w)
{
    switch(w)
    {
    case POWER_WAKE_RESET: return "RESET";
    case POWER_WAKE_TIMER: return "TIMER";
    case POWER_WAKE_MENU_MINUS: return "MENU_MINUS";
    case POWER_WAKE_USER_B: return "USER_B";
    }
    return "?";
}

struct wake_case
{
    const char* causes_name;
    uint32_t    causes;
    const char* pins_name;
    uint64_t    pins;
    power_wake  want;
};

const wake_case WAKE_CASES[] = {
    {"reset", RESET, "none", 0, POWER_WAKE_RESET},
    {"reset", RESET, "15", P15, POWER_WAKE_RESET},
    {"reset", RESET, "16", P16, POWER_WAKE_RESET},
    {"reset", RESET, "15+16", P15 | P16, POWER_WAKE_RESET},
    {"reset+ext1", RESET | EXT1, "16", P16, POWER_WAKE_RESET},
    {"timer", TIMER, "none", 0, POWER_WAKE_TIMER},
    {"timer", TIMER, "15", P15, POWER_WAKE_TIMER},
    {"timer", TIMER, "16", P16, POWER_WAKE_TIMER},
    {"timer", TIMER, "15+16", P15 | P16, POWER_WAKE_TIMER},
    {"ext1", EXT1, "none", 0, POWER_WAKE_TIMER},
    {"ext1", EXT1, "15", P15, POWER_WAKE_MENU_MINUS},
    {"ext1", EXT1, "16", P16, POWER_WAKE_USER_B},
    {"ext1", EXT1, "15+16", P15 | P16, POWER_WAKE_USER_B},
    {"ext1", EXT1, "3", P3, POWER_WAKE_TIMER},
    {"ext1", EXT1, "3+15", P3 | P15, POWER_WAKE_MENU_MINUS},
    {"timer+ext1", TIMER | EXT1, "none", 0, POWER_WAKE_TIMER},
    {"timer+ext1", TIMER | EXT1, "15", P15, POWER_WAKE_MENU_MINUS},
    {"timer+ext1", TIMER | EXT1, "16", P16, POWER_WAKE_USER_B},
    {"timer+ext1", TIMER | EXT1, "15+16", P15 | P16, POWER_WAKE_USER_B},
    {"none", 0, "none", 0, POWER_WAKE_TIMER},
    {"none", 0, "16", P16, POWER_WAKE_TIMER},
    {"ext0", EXT0, "none", 0, POWER_WAKE_TIMER},
};

struct usb_case
{
    int  mv;
    bool want;
};

const usb_case USB_CASES[] = {
    {-1000, true}, {-1, true}, {0, false}, {3400, false}, {4200, false}, {4300, false},
    {4349, false}, {4350, false}, {4351, true}, {4600, true}, {5000, true},
};

struct low_case
{
    bool was_low;
    bool usb;
    int  mv;
    bool want;
};

const low_case LOW_CASES[] = {
    // Battery, not low before: the cutoff at 3400.
    {false, false, 0, true},
    {false, false, 3000, true},
    {false, false, 3399, true},
    {false, false, 3400, false},
    {false, false, 3401, false},
    {false, false, 3599, false},
    {false, false, 3600, false},
    {false, false, 4200, false},
    // Battery, low before: stays low up to 3600.
    {true, false, 3000, true},
    {true, false, 3399, true},
    {true, false, 3400, true},
    {true, false, 3401, true},
    {true, false, 3599, true},
    {true, false, 3600, false},
    {true, false, 3601, false},
    {true, false, 4200, false},
    // USB is never low.
    {false, true, 3000, false},
    {true, true, 3000, false},
    {false, true, 3399, false},
    {true, true, 3599, false},
    // A failed read is not low.
    {false, false, -1, false},
    {true, false, -1, false},
    {true, false, -1000, false},
};

struct arm_case
{
    int         seconds;
    const char* low_name;
    uint64_t    low;
    uint64_t    want_pins;
    int         want_seconds;
};

const arm_case ARM_CASES[] = {
    // Timer sleep: a held pin is left out, the timer stays.
    {60, "none", 0, P15 | P16, 60},
    {60, "15", P15, P16, 60},
    {60, "16", P16, P15, 60},
    {60, "15+16", P15 | P16, 0, 60},
    {1, "15+16", P15 | P16, 0, 1},
    // Other pins low do not matter.
    {60, "3", P3, P15 | P16, 60},
    // Button only sleep: no timer while a pin is armed.
    {0, "none", 0, P15 | P16, 0},
    {0, "15", P15, P16, 0},
    {-5, "16", P16, P15, 0},
    {0, "3+16", P3 | P16, P15, 0},
    // Button only sleep with both pins held: the timer instead.
    {0, "15+16", P15 | P16, 0, POWER_STUCK_WAKE_S},
    {-1, "15+16", P15 | P16, 0, POWER_STUCK_WAKE_S},
};
} // namespace

int main()
{
    char what[160];

    for(const wake_case& c : WAKE_CASES)
    {
        power_wake got = power_decode_wake(c.causes, c.pins);
        snprintf(what, sizeof what, "wake causes %s, ext1 pins %s: %s (got %s)", c.causes_name, c.pins_name,
                 wake_name(c.want), wake_name(got));
        check(got == c.want, what);
    }

    for(const usb_case& c : USB_CASES)
    {
        bool got = power_rail_is_usb(c.mv);
        snprintf(what, sizeof what, "rail %d mV: %s (got %s)", c.mv, c.want ? "USB" : "battery", got ? "USB" : "battery");
        check(got == c.want, what);
    }

    for(const low_case& c : LOW_CASES)
    {
        bool got = power_low_next(c.was_low, c.usb, c.mv);
        snprintf(what, sizeof what, "low: was %s, %s, rail %d mV: %s (got %s)", c.was_low ? "low" : "ok",
                 c.usb ? "USB" : "battery", c.mv, c.want ? "low" : "ok", got ? "low" : "ok");
        check(got == c.want, what);
    }

    // The two together with no USB host: the rail alone decides USB.
    for(int mv : {-1, 4351, 4600})
        for(bool was_low : {false, true})
        {
            bool got = power_low_next(was_low, power_rail_is_usb(mv), mv);
            snprintf(what, sizeof what, "low from rail only: was %s, rail %d mV: ok (got %s)", was_low ? "low" : "ok", mv,
                     got ? "low" : "ok");
            check(!got, what);
        }

    struct judge_case
    {
        bool host;
        int  mv;
        bool was_low, usb, low;
    };
    const judge_case JUDGE_CASES[] = {
        {false, 3300, false, false, true}, {false, 3500, false, false, false}, {false, 3500, true, false, true},
        {false, 3700, true, false, false}, {false, -1, false, true, false},    {false, -1, true, true, false},
        {false, 4600, true, true, false},  {true, 3300, false, true, false},   {true, 3300, true, true, false},
        {true, -1, false, true, false},    {true, 4000, false, true, false},
    };
    for(const judge_case& c : JUDGE_CASES)
    {
        power_reading got = power_judge(c.host, c.mv, c.was_low);
        snprintf(what, sizeof what, "judge: host %d, rail %d mV, was %s: %s, %s (got %d mV, %s, %s)", c.host, c.mv,
                 c.was_low ? "low" : "ok", c.usb ? "USB" : "battery", c.low ? "low" : "ok", got.rail_mv,
                 got.usb ? "USB" : "battery", got.low ? "low" : "ok");
        check(got.rail_mv == c.mv && got.usb == c.usb && got.low == c.low, what);
    }

    struct critical_case
    {
        bool was_critical, usb;
        int  mv;
        bool want;
    };
    const critical_case CRITICAL_CASES[] = {
        // Battery, not critical before: the cutoff at 3300.
        {false, false, 0, true},     {false, false, 3299, true},  {false, false, 3300, false},
        {false, false, 3399, false}, {false, false, 4200, false},
        // Battery, critical before: stays critical up to 3400.
        {true, false, 3300, true},   {true, false, 3399, true},   {true, false, 3400, false},
        {true, false, 4200, false},
        // USB and a failed read are never critical.
        {false, true, 3000, false},  {true, true, 3000, false},   {false, false, -1, false},
        {true, false, -1, false},
    };
    for(const critical_case& c : CRITICAL_CASES)
    {
        bool got = power_critical_next(c.was_critical, c.usb, c.mv);
        snprintf(what, sizeof what, "critical: was %s, %s, rail %d mV: %s (got %s)", c.was_critical ? "critical" : "ok",
                 c.usb ? "USB" : "battery", c.mv, c.want ? "critical" : "ok", got ? "critical" : "ok");
        check(got == c.want, what);
    }

    struct judge2_case
    {
        int  mv;
        bool was_low, was_critical, low, critical;
    };
    const judge2_case JUDGE2_CASES[] = {
        {3250, false, false, true, true},
        {3350, false, false, true, false},
        {3350, false, true, true, true},
        // Out of critical, the low state holds up to 3600 even when low was not kept.
        {3450, false, true, true, false},
        {3599, false, true, true, false},
        {3600, false, true, false, false},
        {3700, true, false, false, false},
    };
    for(const judge2_case& c : JUDGE2_CASES)
    {
        power_reading got = power_judge(false, c.mv, c.was_low, c.was_critical);
        snprintf(what, sizeof what, "judge: rail %d mV, was low %d critical %d: low %d critical %d (got %d, %d)", c.mv,
                 c.was_low, c.was_critical, c.low, c.critical, got.low, got.critical);
        check(!got.usb && got.low == c.low && got.critical == c.critical, what);
    }

    // A critical reading is low too, and USB is neither.
    {
        unsigned bad = 0;
        for(int host = 0; host < 2; host++)
            for(int was = 0; was < 4; was++)
                for(int mv = -100; mv <= 6000; mv++)
                {
                    power_reading r = power_judge(host != 0, mv, (was & 1) != 0, (was & 2) != 0);
                    bad += (r.critical && !r.low) || (r.usb && (r.low || r.critical));
                }
        check(bad == 0, "judge: no rail from -100 to 6000 mV gives critical without low, or USB with either");
    }

    // A badge that sleeps critical and charges on the bench, one reading per wake.
    {
        bool       critical = false, low = false;
        const int  rails[]  = {3350, 3290, 3320, 3390, 3420, 3550, 3650};
        const char want[]   = {'L', 'C', 'C', 'C', 'L', 'L', 'O'};
        int        right    = 0;
        for(int i = 0; i < 7; i++)
        {
            power_reading r = power_judge(false, rails[i], low, critical);
            critical        = r.critical;
            low             = r.low;
            right += (r.critical ? 'C' : r.low ? 'L' : 'O') == want[i];
        }
        check(right == 7, "critical then charging: 3350 low, 3290 to 3390 critical, 3420 and 3550 low, 3650 ok");
    }

    struct early_case
    {
        power_wake wake;
        int        mv;
        bool       host, was_low, was_critical, want;
    };
    const early_case EARLY_CASES[] = {
        {POWER_WAKE_USER_B, 3800, false, false, false, true},
        {POWER_WAKE_MENU_MINUS, 3800, false, false, false, true},
        {POWER_WAKE_USER_B, 3399, false, false, false, false},
        {POWER_WAKE_USER_B, 3400, false, false, false, true},
        // Low kept from the last boot: the supply waits for 3600.
        {POWER_WAKE_USER_B, 3500, false, true, false, false},
        {POWER_WAKE_USER_B, 3600, false, true, false, true},
        {POWER_WAKE_USER_B, 3250, false, false, false, false},
        {POWER_WAKE_USER_B, 3450, false, false, true, false},
        // USB never blocks it.
        {POWER_WAKE_USER_B, 3000, true, true, true, true},
        {POWER_WAKE_USER_B, 4600, false, true, false, true},
        {POWER_WAKE_USER_B, -1, false, true, true, true},
        // Only a button wake.
        {POWER_WAKE_TIMER, 4000, false, false, false, false},
        {POWER_WAKE_RESET, 4000, false, false, false, false},
        {POWER_WAKE_RESET, 4600, true, false, false, false},
    };
    for(const early_case& c : EARLY_CASES)
    {
        bool got = power_early_supply(c.wake, power_judge(c.host, c.mv, c.was_low, c.was_critical));
        snprintf(what, sizeof what, "early supply, wake %s, rail %d mV, host %d, was low %d critical %d: %s (got %s)",
                 wake_name(c.wake), c.mv, c.host, c.was_low, c.was_critical, c.want ? "on" : "off", got ? "on" : "off");
        check(got == c.want, what);
    }

    struct after_case
    {
        int                mv;
        bool               host, was_low;
        power_supply_after want;
    };
    const after_case AFTER_CASES[] = {
        {3800, false, false, POWER_SUPPLY_KEEP},     {3400, false, false, POWER_SUPPLY_KEEP},
        {3399, false, false, POWER_SUPPLY_LOW},      {3300, false, false, POWER_SUPPLY_LOW},
        {3299, false, false, POWER_SUPPLY_CRITICAL}, {3599, false, true, POWER_SUPPLY_LOW},
        {3200, true, false, POWER_SUPPLY_KEEP},      {-1, false, false, POWER_SUPPLY_KEEP},
    };
    for(const after_case& c : AFTER_CASES)
    {
        power_supply_after got = power_supply_check(power_judge(c.host, c.mv, c.was_low));
        snprintf(what, sizeof what, "after the supply, rail %d mV, host %d, was low %d: %d (got %d)", c.mv, c.host,
                 c.was_low, c.want, got);
        check(got == c.want, what);
    }

    // One read gives both results, so USB and low never hold together.
    {
        unsigned both = 0;
        for(int host = 0; host < 2; host++)
            for(int was_low = 0; was_low < 2; was_low++)
                for(int mv = -100; mv <= 6000; mv++)
                {
                    power_reading r = power_judge(host != 0, mv, was_low != 0);
                    both += r.usb && r.low;
                }
        check(both == 0, "judge: no rail from -100 to 6000 mV gives USB and low together");
    }

    struct held_case
    {
        const char* name;
        uint64_t    low, released, want;
    };
    const held_case HELD_CASES[] = {
        // A press made while awake stays armed, so it wakes the chip at once.
        {"16 low, released before", P16, P15 | P16, 0},
        // Held since the button task started: the wake press held through the boot, or a stuck button.
        {"16 low, never released", P16, P15, P16},
        {"15+16 low, 15 released", P15 | P16, P15, P16},
        {"15+16 low, none released", P15 | P16, 0, P15 | P16},
        {"none low", 0, 0, 0},
        // Pins that are not wake pins do not count.
        {"3 low, never released", P3, 0, 0},
    };
    for(const held_case& c : HELD_CASES)
    {
        uint64_t got = power_held_pins(c.low, c.released);
        snprintf(what, sizeof what, "held pins, %s: 0x%llx (got 0x%llx)", c.name, static_cast<unsigned long long>(c.want),
                 static_cast<unsigned long long>(got));
        check(got == c.want, what);
    }

    // A User B press at the start of a timed sleep after a page: it stays armed, so it wakes the badge.
    {
        power_arm got = power_sleep_arm(60, power_held_pins(P16, P15 | P16));
        check(got.pins == (P15 | P16) && got.seconds == 60, "timed sleep, User B pressed while awake: both pins armed");
    }

    for(const arm_case& c : ARM_CASES)
    {
        power_arm got = power_sleep_arm(c.seconds, c.low);
        snprintf(what, sizeof what, "sleep %d s, pins low %s: arm pins 0x%llx timer %d (got 0x%llx, %d)", c.seconds,
                 c.low_name, static_cast<unsigned long long>(c.want_pins), c.want_seconds,
                 static_cast<unsigned long long>(got.pins), got.seconds);
        check(got.pins == c.want_pins && got.seconds == c.want_seconds, what);
    }

    printf("%s, %u failed\n", s_fails ? "FAIL" : "pass", s_fails);
    return s_fails ? 1 : 0;
}
