// The stand in of tone_fx_search() (main/tone_fx_search.h) for host builds and the probes. Integer
// C that models the vector form of main/tone_fx_search.cpp bit for bit, ties included. It reads the
// table and the point limits of main/tone_tables.inc (main/tone_fx_search_internal.h).

#include "tone_fx_model.h"
#include "tone_fx_search.h"
#include "tone_fx_search_internal.h"

namespace
{
using namespace tone_fx_search_internal;

// The key of lane i of a block for the point (pl, pa, pb) from tone_fx_search_point(). Each
// difference is entry minus point, saturated to 16 bits by ee.vsubs.s16. The point's maxima keep it
// at -32767 or above, so the result does not depend on whether the unit saturates a negative
// difference to -32768 (the TRM) or to -32767 (QEMU). A difference above 32767 saturates to 32767 in
// both. The square of a difference is at most 2^30, and the vector form forms it exactly from its
// two 16 bit halves. ee.vadds.s32 saturates (dl^2 + da^2) and then adds db^2 with saturation, which
// is the sum held at 2^31 - 1 since every term is positive. The key is that sum rounded down to a
// multiple of 128 with the entry index in the low 7 bits, so keys are distinct and order as
// (distance, entry index).
inline int32_t key_of(const block& k, int i, int pl, int pa, int pb)
{
    const int32_t dl = sat16(k.l[i] - pl);
    const int32_t da = sat16(k.a[i] - pa);
    const int32_t db = sat16(k.b[i] - pb);
    uint32_t      s  = static_cast<uint32_t>(dl * dl) + static_cast<uint32_t>(da * da);
    if(s > static_cast<uint32_t>(INT32_MAX)) s = INT32_MAX;
    s += static_cast<uint32_t>(db * db);
    if(s > static_cast<uint32_t>(INT32_MAX)) s = INT32_MAX;
    return (static_cast<int32_t>(s) & KEY_MASK) | k.tag[i];
}
} // namespace

void tone_fx_search(q15 L, q15 A, q15 B, uint8_t* best, uint8_t* next)
{
    const table& tab = tone_fx_search_table();
    int          pl, pa, pb;
    tone_fx_search_point(L, A, B, &pl, &pa, &pb);
    int32_t m1[4], m2[4];
    for(int j = 0; j < 4; j++) m1[j] = m2[j] = KEY_NONE;
    // Per lane j, as in the vector form, the keys of entries j and 4 + j of a block are ordered into
    // lo and hi, then merged into the running first and second minimum m1 and m2. The result is the two
    // smallest keys of the lane whatever the order. mn and mx are ee.vmin.s32 and ee.vmax.s32.
    for(int k = 0; k < N_BLOCK; k++)
        for(int j = 0; j < 4; j++)
        {
            const int32_t ka = key_of(tab.blk[k], j, pl, pa, pb);
            const int32_t kb = key_of(tab.blk[k], 4 + j, pl, pa, pb);
            const int32_t lo = mn(ka, kb), hi = mx(ka, kb);
            m2[j]            = mn(m2[j], hi);
            const int32_t t  = mx(m1[j], lo);
            m1[j]            = mn(m1[j], lo);
            m2[j]            = mn(m2[j], t);
        }
    pick(m1, m2, best, next);
}
