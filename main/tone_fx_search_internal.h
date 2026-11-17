// The table of tone_fx_search() and the steps its kernel and its stand in share. For
// tone_fx_search.cpp, the stand in (host_tests/tone_fx/model) and the tests only. The distance, the
// keys and the ranking are described in tone_fx_search.h and tone_fx_search.cpp.
#pragma once

#include <climits>
#include <cstdint>

#include "tone_fx.h"
#include "tonemap.h"

namespace tone_fx_search_internal
{
const int     N_ENTRY  = static_cast<int>(TONEMAP_COUNT);
const int     N_BLOCK  = (N_ENTRY + 7) / 8;
const int32_t KEY_NONE = INT32_MAX;
// Clears the 7 low bits of a distance for the entry index.
const int32_t KEY_MASK = ~127;

static_assert(N_ENTRY >= 2, "the search returns two entries");
static_assert(8 * N_BLOCK <= 128, "a key holds the entry index in 7 bits");
static_assert(TONEMAP_CIE_CHROMA_WEIGHT == 2.0f, "the kernel applies the chroma weight as a doubling");

// 8 entries, in the order the vector form loads them. Entry 8 k + i is lane i of block k, so the
// table is in entry order and needs no map back to entry indices.
struct block
{
    q15 a[8]; // entry a times the chroma weight, q15
    q15 l[8]; // entry L, q15
    q15 b[8]; // entry b times the chroma weight, q15
    // The entry index, which goes into the low 7 bits of the key. A padding lane holds KEY_NONE,
    // which turns its key into KEY_NONE whatever its distance, so no point reaches a padding lane
    // before every entry.
    int32_t tag[8];
};
static_assert(sizeof(block) == 80, "the vector form steps through a block in 16 byte loads");

struct table
{
    int32_t none[4]; // KEY_NONE, the start of both running minima
    int32_t mask[4]; // KEY_MASK
    block   blk[N_BLOCK];
};

inline int sat16(int v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

inline int32_t mn(int32_t x, int32_t y)
{
    return x <= y ? x : y;
}

inline int32_t mx(int32_t x, int32_t y)
{
    return x >= y ? x : y;
}

// The two smallest keys in the 4 lanes of the running minima m1 and m2, as entry indices. Entry e
// sits in lane e mod 4. The best is the smallest m1. The second is the smaller of the second
// smallest m1 and the best lane's m2, since a lane's m2 is above its m1. The scalar core has min and
// max, so this compiles without a branch.
inline void pick(const int32_t* m1, const int32_t* m2, uint8_t* best, uint8_t* next)
{
    const int32_t lo01 = mn(m1[0], m1[1]), hi01 = mx(m1[0], m1[1]);
    const int32_t lo23 = mn(m1[2], m1[3]), hi23 = mx(m1[2], m1[3]);
    const int32_t k1   = mn(lo01, lo23);
    const int32_t s1   = mn(mx(lo01, lo23), mn(hi01, hi23));
    const int32_t k2   = mn(s1, m2[k1 & 3]);
    *best              = static_cast<uint8_t>(k1 & 127);
    *next              = static_cast<uint8_t>(k2 & 127);
}
} // namespace tone_fx_search_internal

// The table the kernel reads, from main/tone_tables.inc.
const tone_fx_search_internal::table& tone_fx_search_table();

// The point (L, A, B) as the search uses it: L, 2A and 2B saturated to 16 bits and held at the
// maxima of main/tone_tables.inc.
void tone_fx_search_point(q15 L, q15 A, q15 B, int* pl, int* pa, int* pb);
