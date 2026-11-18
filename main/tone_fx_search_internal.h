// Shared data layout and key encoding for the nearest-tone search over the tonemap entries.
// The device search, the host table build and the host model all include this file so the three
// agree on one format.
// The table holds the entries in blocks of eight, with the L, a and b components in separate lanes.
// Each entry keeps its own index in a tag field, or KEY_NONE when the slot is padding past N_ENTRY.
// A key combines the squared distance from the query point in its high bits with the entry index in
// its low seven bits.
// KEY_MASK clears those seven bits of the distance to leave room for the index, which fits because
// there are at most 128 entries.
// The smallest key over all entries names the nearest one, and & 127 on it recovers that entry's
// index.
// A padding slot carries KEY_NONE in its tag, which sets all the low bits, so its key stays at the
// maximum and never wins the minimum.
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
const int32_t KEY_MASK = ~127;

static_assert(N_ENTRY >= 2, "the search returns two entries");
static_assert(8 * N_BLOCK <= 128, "a key holds the entry index in 7 bits");
static_assert(TONEMAP_CHROMA_WEIGHT == 2.0f, "the kernel applies the chroma weight as a doubling");

struct block
{
    q15 a[8];
    q15 l[8];
    q15 b[8];
    int32_t tag[8];
};
static_assert(sizeof(block) == 80, "the vector form steps through a block in 16 byte loads");

struct table
{
    int32_t none[4];
    int32_t mask[4];
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
}

const tone_fx_search_internal::table& tone_fx_search_table();

void tone_fx_search_point(q15 L, q15 A, q15 B, int* pl, int* pa, int* pb);
