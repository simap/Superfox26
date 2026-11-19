// Host test of main/browser_logic.h: the page after a wake, the sleep time after a page, the low
// battery step, the offset of a tab selection and what a selection does after its hold. Usage: browser_power_test. Exit status 0 when every check passes.

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "browser_logic.h"

namespace
{
unsigned s_fails = 0;

void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if(!ok) s_fails++;
}

const char* wake_name(power_wake w)
{
    switch(w)
    {
    case POWER_WAKE_RESET: return "reset";
    case POWER_WAKE_TIMER: return "timer";
    case POWER_WAKE_MENU_MINUS: return "Menu -";
    case POWER_WAKE_USER_B: return "User B";
    }
    return "?";
}

const char* action_name(browser_low_action a)
{
    switch(a)
    {
    case BROWSER_RUN: return "run";
    case BROWSER_LOW_DRAW: return "draw";
    case BROWSER_LOW_SLEEP: return "sleep";
    case BROWSER_CRITICAL_SLEEP: return "critical sleep";
    }
    return "?";
}

struct index_case
{
    power_wake wake;
    int        index, shown, n;
    int        want;
};

const index_case INDEX_CASES[] = {
    // The usual state after a page: shown is one before index.
    {POWER_WAKE_RESET, 3, 2, 5, 3},
    {POWER_WAKE_RESET, 0, -1, 5, 0},
    {POWER_WAKE_TIMER, 3, 2, 5, 3},
    {POWER_WAKE_USER_B, 3, 2, 5, 3},
    {POWER_WAKE_MENU_MINUS, 3, 2, 5, 2},
    // Wrap at the end of the list.
    {POWER_WAKE_TIMER, 0, 4, 5, 0},
    {POWER_WAKE_USER_B, 0, 4, 5, 0},
    {POWER_WAKE_MENU_MINUS, 0, 4, 5, 4},
    // After a failed page index has moved on from shown.
    {POWER_WAKE_TIMER, 4, 2, 5, 4},
    {POWER_WAKE_USER_B, 4, 2, 5, 3},
    {POWER_WAKE_MENU_MINUS, 4, 2, 5, 2},
    // One page.
    {POWER_WAKE_TIMER, 0, 0, 1, 0},
    {POWER_WAKE_USER_B, 0, 0, 1, 0},
    {POWER_WAKE_MENU_MINUS, 0, 0, 1, 0},
    // No page shown: User B and Menu - take the next page.
    {POWER_WAKE_USER_B, 2, -1, 5, 2},
    {POWER_WAKE_MENU_MINUS, 2, -1, 5, 2},
    // Kept values out of range, as after the list got shorter.
    {POWER_WAKE_TIMER, 7, 6, 5, 0},
    {POWER_WAKE_TIMER, -3, 2, 5, 0},
    {POWER_WAKE_USER_B, 7, 6, 5, 0},
    {POWER_WAKE_MENU_MINUS, 7, 6, 5, 0},
    {POWER_WAKE_MENU_MINUS, 1, 9, 5, 1},
    {POWER_WAKE_RESET, 7, 6, 5, 0},
};

struct sleep_case
{
    int     page_s;
    int64_t elapsed_ms;
    int     want;
};

const sleep_case SLEEP_CASES[] = {
    {60, 0, 60},
    {60, 20000, 40},
    {60, 20400, 40},
    {60, 20500, 40},
    {60, 20501, 39},
    {60, 58000, 2},
    {60, 59000, 1},
    {60, 59600, 1},
    {60, 60000, 1},
    {60, 90000, 1},
    {1, 0, 1},
    {0, 0, 1},
    {3600, 25000, 3575},
    {86400, 30000, 86370},
    // Clamped to BROWSER_SLEEP_MAX_S.
    {86431, 30000, 86400},
    {100000, 0, 86400},
    {2147483647, 0, 86400},
    {2147483647, 25000, 86400},
};

struct low_case
{
    bool               low, shown, critical;
    browser_low_action want;
    bool               want_low, want_shown;
};

const low_case LOW_CASES[] = {
    {false, false, false, BROWSER_RUN, false, false},
    {false, true, false, BROWSER_RUN, false, false},
    {true, false, false, BROWSER_LOW_DRAW, true, true},
    {true, true, false, BROWSER_LOW_SLEEP, true, true},
    // Critical: no draw, the notice flag stays as it was.
    {true, false, true, BROWSER_CRITICAL_SLEEP, true, false},
    {true, true, true, BROWSER_CRITICAL_SLEEP, true, true},
};
} // namespace

int main()
{
    char what[160];

    for(const index_case& c : INDEX_CASES)
    {
        int got = browser_wake_index(c.wake, c.index, c.shown, c.n);
        snprintf(what, sizeof what, "wake %s, index %d, shown %d, %d pages: page %d (got %d)", wake_name(c.wake), c.index,
                 c.shown, c.n, c.want, got);
        check(got == c.want, what);
    }

    for(const sleep_case& c : SLEEP_CASES)
    {
        int got = browser_sleep_s(c.page_s, c.elapsed_ms);
        snprintf(what, sizeof what, "page %d s, cycle %lld ms: sleep %d s (got %d)", c.page_s,
                 static_cast<long long>(c.elapsed_ms), c.want, got);
        check(got == c.want, what);
    }

    for(const low_case& c : LOW_CASES)
    {
        browser_low_step got = browser_low_next(c.low, c.shown, c.critical);
        snprintf(what, sizeof what, "reading %s, notice %s: %s, low %d, shown %d (got %s, %d, %d)",
                 c.critical ? "critical" : c.low ? "low" : "ok",
                 c.shown ? "shown" : "not shown", action_name(c.want), c.want_low, c.want_shown, action_name(got.action),
                 got.low, got.shown);
        check(got.action == c.want && got.low == c.want_low && got.shown == c.want_shown, what);
    }

    struct offset_case
    {
        int target, base, n, want;
    };
    const offset_case OFFSET_CASES[] = {
        {3, 2, 5, 1},   {1, 2, 5, -1},  {2, 2, 5, 0},   {4, 2, 5, 2},   {0, 2, 5, -2},
        // The short way round the end of the list.
        {0, 4, 5, 1},   {4, 0, 5, -1},
        // An even list: half way round counts forward.
        {3, 0, 6, 3},   {4, 0, 6, -2},  {1, 0, 2, 1},   {0, 0, 1, 0},
        // 47 pages: at most 23 either way.
        {23, 0, 47, 23}, {24, 0, 47, -23}, {46, 0, 47, -1}, {0, 46, 47, 1},
    };
    for(const offset_case& c : OFFSET_CASES)
    {
        int got = browser_offset(c.target, c.base, c.n);
        snprintf(what, sizeof what, "offset of page %d from %d of %d: %d (got %d)", c.target, c.base, c.n, c.want, got);
        check(got == c.want, what);
    }
    {
        int worst = 0;
        for(int t = 0; t < 47; t++)
            for(int b = 0; b < 47; b++)
            {
                int o = browser_offset(t, b, 47);
                worst = std::max(worst, o < 0 ? -o : o);
                if(browser_wrap(b + o, 47) != t) worst = 99;
            }
        check(worst == 23, "47 pages: every offset leads back to its page and is at most 23");
    }
    check(browser_wrap(-1, 5) == 4 && browser_wrap(-11, 5) == 4 && browser_wrap(12, 5) == 2 && browser_wrap(3, 0) == 0,
          "wrap: -1, -11 and 12 in 5 pages, any index in none");

    struct sel_case
    {
        const char*        name;
        bool               open, due, force;
        int                target, base, loading;
        browser_sel_action want;
    };
    const sel_case SEL_CASES[] = {
        {"closed", false, true, false, 3, 2, -1, BROWSER_SEL_NONE},
        {"hold runs", true, false, false, 3, 2, -1, BROWSER_SEL_WAIT},
        {"one forward in the wait", true, true, false, 3, 2, -1, BROWSER_SEL_LOAD},
        {"back to the page shown", true, true, false, 2, 2, -1, BROWSER_SEL_DROP},
        {"the page that loads", true, true, false, 3, 2, 3, BROWSER_SEL_DROP},
        {"past the page that loads", true, true, false, 4, 2, 3, BROWSER_SEL_LOAD},
        {"back from the page that loads", true, true, false, 1, 2, 3, BROWSER_SEL_LOAD},
        {"back to the page shown during a render", true, true, false, 2, 2, 3, BROWSER_SEL_DROP},
        {"Menu - wake, the page shown", true, true, true, 2, 2, -1, BROWSER_SEL_LOAD},
        {"console, the page that loads", true, true, true, 3, 2, 3, BROWSER_SEL_LOAD},
        {"forced, hold runs", true, false, true, 2, 2, -1, BROWSER_SEL_WAIT},
    };
    for(const sel_case& c : SEL_CASES)
    {
        browser_sel_action got = browser_sel_next(c.open, c.due, c.force, c.target, c.base, c.loading);
        snprintf(what, sizeof what, "selection, %s: %d (got %d)", c.name, c.want, got);
        check(got == c.want, what);
    }

    // The badge stays in the low state across wakes until the reading is ok: draw once, then sleep.
    {
        bool shown  = false;
        int  draws  = 0;
        int  sleeps = 0;
        for(int wake = 0; wake < 4; wake++)
        {
            browser_low_step s = browser_low_next(true, shown);
            draws += s.action == BROWSER_LOW_DRAW;
            sleeps += s.action != BROWSER_RUN;
            shown = s.shown;
        }
        browser_low_step s = browser_low_next(false, shown);
        check(draws == 1 && sleeps == 4 && s.action == BROWSER_RUN && !s.shown,
              "four low wakes then an ok one: one draw, four sleeps, then run with the flags cleared");
    }

    // A badge that goes critical before its first low draw never draws, and draws once when it climbs
    // back to low.
    {
        bool shown = false;
        int  draws = 0;
        for(int wake = 0; wake < 3; wake++)
        {
            browser_low_step s = browser_low_next(true, shown, true);
            draws += s.action == BROWSER_LOW_DRAW;
            shown = s.shown;
        }
        browser_low_step s = browser_low_next(true, shown, false);
        check(draws == 0 && s.action == BROWSER_LOW_DRAW,
              "three critical wakes then a low one: no draw while critical, then the notice once");
    }

    printf("%s, %u failed\n", s_fails ? "FAIL" : "pass", s_fails);
    return s_fails ? 1 : 0;
}
