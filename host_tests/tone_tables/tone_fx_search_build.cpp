// The entry table of the search kernel (main/tone_fx_search.cpp) and the limits of its point, from
// the tone table. main/tone_fx_search_internal.h describes the layout.

#include "tone_fx_search_internal.h"
#include "tone_tables_build.h"

namespace
{
using namespace tone_fx_search_internal;

// Real numbers outside -1 to 1 do not fit q15. 1 itself is stored as Q15_ONE.
bool fits(float v)
{
    return v >= -1.0f && v <= 1.0f;
}
} // namespace

// The highest L, 2A and 2B of a point are 32767 plus the lowest entry L, 2a and 2b when that is
// negative. The search subtracts the point from the entry, so a point held there keeps every
// difference at -32767 or above.
bool tone_fx_search_build(tone_fx_search_internal::table* tab, int max[3])
{
    max[0] = max[1] = max[2] = 32767;
    for(int j = 0; j < 4; j++) tab->none[j] = KEY_NONE, tab->mask[j] = KEY_MASK;
    for(int k = 0; k < N_BLOCK; k++)
    {
        block& b = tab->blk[k];
        for(int i = 0; i < 8; i++)
        {
            const int e = 8 * k + i;
            if(e >= N_ENTRY)
            {
                b.l[i] = b.a[i] = b.b[i] = 0;
                b.tag[i]                 = KEY_NONE;
                continue;
            }
            const tone_entry& t  = k_tonemap[e];
            const float       wa = TONEMAP_CHROMA_WEIGHT * t.a;
            const float       wb = TONEMAP_CHROMA_WEIGHT * t.b;
            if(!fits(t.L) || !fits(wa) || !fits(wb)) return false;
            b.l[i]   = q15_from(t.L);
            b.a[i]   = q15_from(wa);
            b.b[i]   = q15_from(wb);
            b.tag[i] = e;
            if(32767 + b.l[i] < max[0]) max[0] = 32767 + b.l[i];
            if(32767 + b.a[i] < max[1]) max[1] = 32767 + b.a[i];
            if(32767 + b.b[i] < max[2]) max[2] = 32767 + b.b[i];
        }
    }
    return true;
}
