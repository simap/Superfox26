// Host test of main/leds_logic.h: the bar for a tab offset, the blink, the countdown and load bars and
// how the layers combine. Usage: leds_test. Exit status 0 when every check passes.
// A frame is written as 8 letters, left to right: . dark, a amber, t teal, v violet, w white, b blue,
// r red, g green.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "leds_logic.h"

namespace
{
unsigned s_fails = 0;

void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if(!ok) s_fails++;
}

void text(const leds_frame& f, char* out)
{
    static const char LETTERS[] = ".atvwbrg";
    for(int i = 0; i < LEDS_N; i++) out[i] = LETTERS[f.px[i]];
    out[LEDS_N] = 0;
}

void check_frame(const leds_frame& f, const char* want, const char* name)
{
    char got[LEDS_N + 1];
    text(f, got);
    char what[160];
    snprintf(what, sizeof what, "%s: %s (got %s)", name, want, got);
    check(!strcmp(got, want), what);
}

struct offset_case
{
    int         offset;
    const char* want;
};

const offset_case OFFSET_CASES[] = {
    {0, "...vv..."},  {1, "v......."},  {2, "vv......"},  {7, "vvvvvvv."},  {8, "w......."},
    {14, "wwwwwww."}, {15, "b......."}, {21, "bbbbbbb."}, {22, "g......."}, {23, "gg......"},
    {28, "ggggggg."}, {40, "ggggggg."}, {-1, ".......v"}, {-3, ".....vvv"}, {-7, ".vvvvvvv"},
    {-8, ".......w"}, {-23, "......gg"},
};
} // namespace

int main()
{
    char what[160];

    for(const offset_case& c : OFFSET_CASES)
    {
        snprintf(what, sizeof what, "offset %d", c.offset);
        check_frame(leds_offset_frame(c.offset), c.want, what);
    }

    // Every offset from -28 to 28 has its own bar, so 57 pages can be told apart. None is a full bar,
    // so none looks like a full load or countdown bar.
    {
        unsigned same = 0, full = 0;
        for(int a = -28; a <= 28; a++)
        {
            leds_frame f = leds_offset_frame(a);
            full += f.px[0] != LEDS_DARK && f.px[LEDS_N - 1] != LEDS_DARK;
            for(int b = a + 1; b <= 28; b++) same += leds_same(f, leds_offset_frame(b));
        }
        check(same == 0 && full == 0, "offsets -28 to 28: no two bars alike, no bar lit at both ends");
    }

    struct lit_case
    {
        int64_t done, total;
        int     want;
    };
    const lit_case LIT_CASES[] = {
        {1, 8, 1}, {8, 8, 8}, {0, 8, 0}, {1, 60000, 1}, {59999, 60000, 8}, {7500, 60000, 1}, {7501, 60000, 2},
        {5, 0, 0}, {-5, 10, 0}, {20, 10, 8}, {86400000, 86400000, 8},
    };
    for(const lit_case& c : LIT_CASES)
    {
        int got = leds_ceil_lit(c.done, c.total);
        snprintf(what, sizeof what, "lit for %lld of %lld: %d (got %d)", static_cast<long long>(c.done),
                 static_cast<long long>(c.total), c.want, got);
        check(got == c.want, what);
    }

    check(!leds_blink_off(0) && !leds_blink_off(LEDS_BLINK_US - 1) && leds_blink_off(LEDS_BLINK_US) &&
              !leds_blink_off(2 * LEDS_BLINK_US) && !leds_blink_off(-1),
          "blink: on from the press for one half period, then off, then on");

    struct compose_case
    {
        const char* name;
        leds_layers l;
        const char* want;
    };
    const compose_case COMPOSE_CASES[] = {
        {"off", {}, "........"},
        {"load 3", {LEDS_AMBER, 3, false, false, false, 0, false, false}, "aaa....."},
        {"countdown 5", {LEDS_TEAL, 5, true, false, false, 0, false, false}, "ttttt..."},
        {"countdown held", {LEDS_TEAL, 2, true, true, false, 0, false, false}, "tttttttt"},
        {"countdown held at its end", {LEDS_TEAL, 0, true, true, false, 0, false, false}, "tttttttt"},
        {"load held", {LEDS_AMBER, 3, false, true, false, 0, false, false}, "aaa....t"},
        {"draw held", {LEDS_AMBER, 8, false, true, false, 0, false, false}, "aaaaaaat"},
        {"off held", {LEDS_DARK, 0, false, true, false, 0, false, false}, ".......t"},
        {"selection over the countdown", {LEDS_TEAL, 5, true, false, true, 2, false, false}, "vv......"},
        {"selection over a load, blink on", {LEDS_AMBER, 8, false, false, true, -1, false, false}, ".......v"},
        {"selection over a load, blink off", {LEDS_AMBER, 8, false, false, true, -1, true, false}, "aaaaaaaa"},
        {"selection over a held load, blink off", {LEDS_AMBER, 4, false, true, true, 3, true, false}, "aaaa...t"},
        {"selection, stay", {LEDS_AMBER, 2, false, false, true, 0, false, false}, "...vv..."},
        {"flash over all", {LEDS_AMBER, 8, false, true, true, 3, false, true}, "w......w"},
    };
    for(const compose_case& c : COMPOSE_CASES) check_frame(leds_compose(c.l), c.want, c.name);

    printf("%s, %u failed\n", s_fails ? "FAIL" : "pass", s_fails);
    return s_fails ? 1 : 0;
}
