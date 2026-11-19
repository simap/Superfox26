// The builds of the generator of main/tone_tables.inc (tone_tables_gen.cpp), and what it shares with
// the check of its pass A tables (tone_tables_check.cpp). Each build is the computation the firmware
// would otherwise run at boot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "tone_fx_search_internal.h"
#include "tone_photo.h"

// The squeeze parameters into k, K_COUNT of them, and the largest miss of each fitted edge against
// its float table. False when a parameter does not fit q15 or a fit misses by more than 0.0005.
bool tone_fx_squeeze_build(int16_t k[], float* err_img, float* err_pan);

// The neutral list and the bracket step table, read through the accessors of tone_fx_bracket.h and
// tone_fx_bracket_internal.h. False for no neutral entry, more than TONE_FX_NEUTRAL_MAX, an entry
// index above 127 or more than 2 TONE_FX_NEUTRAL_MAX steps.
bool tone_fx_bracket_build();

// The search table and the highest L, 2A and 2B of a point. False when an entry's L, 2a or 2b lies
// outside -1 to 1.
bool tone_fx_search_build(tone_fx_search_internal::table* tab, int max[3]);

// The Delaunay triangulation of the entries e with use[e], at yv[2 e] and yv[2 e + 1]
// (tone_photo_yv() of their linear light), into out, each triangle's entries darkest first, and the entries of the upper hull
// into top from black to paper (tone_photo_clamp()). False, with the reason in why, on a flat
// triangle, a hole in the hull, more triangles than a byte indexes or an upper hull that does not
// run from neutral to neutral by rising Y.
bool tone_photo_build(const int16_t* yv, const bool* use, int n, std::vector<tone_tri>& out,
                      std::vector<uint8_t>& top, char* why, size_t len);

// The triangle of tri that holds (y, v), or -1.
int tone_photo_locate(const std::vector<tone_tri>& tri, const int16_t* yv, int y, int v);

// A coloured target after pass A: its point after tone_photo_clamp() and its cell
// (tone_photo_cell()).
struct tone_photo_sample
{
    int y, v, cell;
};

// Per cell, the triangle that holds most of the samples of that cell, else that of the nearest cell
// with samples. outside counts the samples outside every triangle. False with no sample inside.
bool tone_photo_grid(const std::vector<tone_tri>& tri, const int16_t* yv, const std::vector<tone_photo_sample>& s,
                     uint8_t grid[], int* outside);

// What pass A of tones.cpp gives a pixel of gray byte v: the squeezed L, and the entry when the
// slot is on one (kind K_ENTRY), else -1.
struct tone_gray_target
{
    int16_t L;
    int     on;
};

// tones_solid_gray() for every gray byte g into out[g]: of the grays whose target is on a neutral
// entry, the one whose entry is nearest to g's target in L, then nearest to g. The distance is the
// distance in L plus 1e-6 per gray step, in q15. qL holds each entry's L in q15.
inline void tone_solid_gray_table(const tone_gray_target t[256], const int16_t* qL, uint8_t out[256])
{
    for(int g = 0; g < 256; g++)
    {
        int     best = g;
        int64_t bd   = INT64_MAX;
        for(int v = 0; v < 256; v++)
        {
            if(t[v].on < 0) continue;
            const int64_t d = static_cast<int64_t>(abs(qL[t[v].on] - t[g].L)) * 1000000 +
                              static_cast<int64_t>(abs(v - g)) * 32768;
            if(d < bd) bd = d, best = v;
        }
        out[g] = static_cast<uint8_t>(best);
    }
}
