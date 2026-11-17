// The neutral list and the step table of the bracket kernel (main/tone_fx_bracket.cpp), from the
// tone table. The kernel's file describes the table.
//
// The build finds every L where the pair changes by running the comparisons of bracket() of
// main/tones_float_ref.inc in float on each q15 value, so the steps fall exactly where the float
// code puts them. In the generator's link this file stands in for main/tone_fx_bracket.cpp: it
// defines the accessors of tone_fx_bracket.h and tone_fx_bracket_internal.h, so the stand in of the
// kernel reads the table built here.

#include <algorithm>
#include <cstring>

#include "tone_fx_bracket.h"
#include "tone_fx_bracket_internal.h"
#include "tone_tables_build.h"
#include "tonemap.h"

namespace
{
// The same value as ON_ENTRY_L in tones_float_ref.inc. The kernel matches bracket() only while the
// two agree.
const float ON_ENTRY_L = 0.002f;

const int MAX_STEPS = 2 * TONE_FX_NEUTRAL_MAX;

uint8_t s_neutral[TONE_FX_NEUTRAL_MAX];
int     s_n_neutral = 0;
int     s_n_steps   = 0; // padded to a multiple of 8

alignas(16) int16_t s_table[2 + 2 * MAX_STEPS];
} // namespace

bool tone_fx_bracket_build()
{
    uint8_t list[TONEMAP_COUNT];
    int     n = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(!strpbrk(k_tonemap[e].pattern, "rR")) list[n++] = static_cast<uint8_t>(e);
    if(n == 0 || n > TONE_FX_NEUTRAL_MAX) return false;
    for(int i = 0; i < n; i++)
        if(list[i] > 127) return false;
    // Lightest first. tones_float_ref.inc sorts the same way. Equal L keeps table order here.
    std::stable_sort(list, list + n, [](uint8_t a, uint8_t b) { return k_tonemap[a].L > k_tonemap[b].L; });
    float ln[TONE_FX_NEUTRAL_MAX];
    for(int i = 0; i < n; i++) ln[i] = k_tonemap[list[i]].L;

    // bracket() takes k as the first neutral entry not above L + ON_ENTRY_L, at most n - 1. As L
    // rises that k only falls, so one pass carries it.
    int     k = n - 1, steps = 0;
    int16_t prev = 0;
    int16_t t[MAX_STEPS], p[MAX_STEPS];
    for(int q = 0; q < 32768; q++)
    {
        const float L = static_cast<float>(q) * (1.0f / 32768.0f);
        while(k > 0 && !(ln[k - 1] > L + ON_ENTRY_L)) k--;
        const int     lo = list[k];
        const int     hi = k == 0 || L - ln[k] <= ON_ENTRY_L ? lo : list[k - 1];
        const int16_t s  = static_cast<int16_t>(lo | hi << 8);
        if(q > 0 && s != prev)
        {
            if(steps == MAX_STEPS) return false;
            t[steps] = static_cast<int16_t>(q);
            p[steps] = static_cast<int16_t>(prev - s);
            steps++;
        }
        prev = s;
    }
    s_table[0] = prev;
    s_table[1] = 0;
    // Highest threshold first, then pad.
    int m = 0;
    for(int i = steps - 1; i >= 0; i--, m++) s_table[2 + 2 * m] = t[i], s_table[3 + 2 * m] = p[i];
    for(; m % 8; m++) s_table[2 + 2 * m] = -32768, s_table[3 + 2 * m] = 0;
    s_n_steps = m;
    memcpy(s_neutral, list, n);
    s_n_neutral = n;
    return true;
}

int tone_fx_neutral_count()
{
    return s_n_neutral;
}

const uint8_t* tone_fx_neutral_entries()
{
    return s_neutral;
}

int tone_fx_bracket_steps()
{
    return s_n_steps;
}

const int16_t* tone_fx_bracket_table()
{
    return s_table;
}
