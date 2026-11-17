// Host test of main/tone_fx_bracket.cpp on the tables of main/tone_tables.inc. Run from this
// directory: make test.
//
// 1. The neutral list of the file against the list the float dither builds.
// 2. tone_fx_bracket, as its stand in ../model/tone_fx_bracket_model.cpp, on every int16 value of L
//    against bracket() of tones.cpp in float on L / 32768. Every difference is listed with its L.
// 3. The instruction sequence of the vector kernel on a model of the vector instructions
//    (pie_emu.h) against the stand in on every int16 value, with and without an 8 value tail.
// Exit status 1 when any check differs.
//
// The reference below is the neutral list and bracket() of main/tones.cpp, copied: tones.cpp needs
// the ESP-IDF and panel headers.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "pie_emu.h"
#include "tone_fx_bracket.h"
#include "tone_fx_bracket_internal.h"
#include "tonemap.h"

namespace
{
// From tones.cpp.
const float ON_ENTRY_L = 0.002f;
uint8_t     s_neutrals[TONEMAP_COUNT];
float       s_neutral_l[TONEMAP_COUNT];
int         s_n_neutral = 0;

void ref_init()
{
    s_n_neutral = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
    {
        if(strpbrk(k_tonemap[e].pattern, "rR")) continue;
        s_neutrals[s_n_neutral++] = static_cast<uint8_t>(e);
    }
    std::sort(s_neutrals, s_neutrals + s_n_neutral,
              [](uint8_t a, uint8_t b) { return k_tonemap[a].L > k_tonemap[b].L; });
    for(int k = 0; k < s_n_neutral; k++) s_neutral_l[k] = k_tonemap[s_neutrals[k]].L;
}

void bracket(float L, uint8_t* lo, uint8_t* hi)
{
    int k = 0;
    while(k < s_n_neutral - 1 && s_neutral_l[k] > L + ON_ENTRY_L) k++;
    *lo = s_neutrals[k];
    *hi = k == 0 || L - s_neutral_l[k] <= ON_ENTRY_L ? *lo : s_neutrals[k - 1];
}

// bracket_pie of tone_fx_bracket.cpp, instruction for instruction.
void bracket_emu(const q15* L, int pairs, uint8_t* lo, uint8_t* hi)
{
    pie            e;
    const int16_t* base   = tone_fx_bracket_table();
    const int      groups = tone_fx_bracket_steps() / 8;
    do
    {
        e.vld(0, L, 16);
        const int16_t* tp = base;
        e.vld(1, L, 16);
        int g = groups;
        e.vldbc16(2, tp, 4);
        e.orq(3, 2, 2);
        do
        {
            for(int s = 0; s < 8; s++)
            {
                e.vldbc16(4, tp, 2);
                e.vldbc16(5, tp, 2);
                e.vcmplt16(6, 0, 4);
                e.vcmplt16(7, 1, 4);
                e.andq(6, 6, 5);
                e.andq(7, 7, 5);
                e.vadds16(2, 2, 6);
                e.vadds16(3, 3, 7);
            }
        } while(--g);
        e.vunzip8(2, 3);
        e.vst(2, lo, 16);
        e.vst(3, hi, 16);
    } while(--pairs);
}

// tone_fx_bracket of tone_fx_bracket.cpp with bracket_emu.
void bracket_pie_emu(const q15* L, int n, uint8_t* lo, uint8_t* hi)
{
    if(n >= 16) bracket_emu(L, n / 16, lo, hi);
    if(n % 16)
    {
        alignas(16) q15     tl[16];
        alignas(16) uint8_t tlo[16], thi[16];
        const int           at = n - 8;
        memcpy(tl, L + at, 8 * sizeof(q15));
        memcpy(tl + 8, L + at, 8 * sizeof(q15));
        bracket_emu(tl, 1, tlo, thi);
        memcpy(lo + at, tlo, 8);
        memcpy(hi + at, thi, 8);
    }
}
} // namespace

int main()
{
    int fail = 0;
    ref_init();

    // 1. The neutral list.
    const int      n_neutral = tone_fx_neutral_count();
    const uint8_t* neutral   = tone_fx_neutral_entries();
    bool           same      = n_neutral == s_n_neutral;
    for(int k = 0; same && k < n_neutral; k++) same = neutral[k] == s_neutrals[k];
    printf("neutral entries: %d, %s the list of tones_init()\n", n_neutral, same ? "same as" : "DIFFERENT FROM");
    printf("neutral entries, lightest first, entry (L):");
    for(int k = 0; k < n_neutral; k++) printf(" %d (%.4f)", neutral[k], static_cast<double>(k_tonemap[neutral[k]].L));
    printf("\n");
    printf("steps per value: %d after padding to a multiple of 8\n", tone_fx_bracket_steps());
    fail |= !same;

    // 2 and 3. Every int16 value, in arrays of CH.
    const int                  CH = 4096;
    alignas(16) static q15     L[CH];
    alignas(16) static uint8_t lo_c[CH], hi_c[CH], lo_e[CH], hi_e[CH];
    int                        differ_ref = 0, differ_ref_q15 = 0, differ_emu = 0;
    for(int v0 = -32768; v0 < 32768; v0 += CH)
    {
        for(int i = 0; i < CH; i++) L[i] = static_cast<q15>(v0 + i);
        tone_fx_bracket(L, CH, lo_c, hi_c);
        // The emulated vector form runs the chunk as CH - 8 values, which ends in an 8 value
        // tail, then the last 8 values as a call of 8.
        bracket_pie_emu(L, CH - 8, lo_e, hi_e);
        alignas(16) uint8_t lo8[16], hi8[16];
        bracket_pie_emu(L + CH - 8, 8, lo8, hi8);
        memcpy(lo_e + CH - 8, lo8, 8);
        memcpy(hi_e + CH - 8, hi8, 8);
        for(int i = 0; i < CH; i++)
        {
            uint8_t rlo, rhi;
            bracket(q15_to(L[i]), &rlo, &rhi);
            if(rlo != lo_c[i] || rhi != hi_c[i])
            {
                if(differ_ref < 40)
                    printf("L %d (%.6f): kernel lo %d hi %d, bracket() lo %d hi %d\n", L[i],
                           static_cast<double>(q15_to(L[i])), lo_c[i], hi_c[i], rlo, rhi);
                differ_ref++;
                differ_ref_q15 += L[i] >= 0;
            }
            differ_emu += (lo_e[i] != lo_c[i]) + (hi_e[i] != hi_c[i]);
        }
    }
    printf("against bracket() in float: %d of 65536 values differ, %d of them in 0 to 32767\n", differ_ref,
           differ_ref_q15);
    printf("the vector sequence on the instruction model against the stand in: %d lo and hi bytes differ\n", differ_emu);
    fail |= differ_ref != 0 || differ_emu != 0;

    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
