// Nearest and second nearest tone entry to a point in Oklab, in fixed point, over all entries.
//
// Distance. The weighted Oklab distance of tones.cpp, dl^2 + (2 da)^2 + (2 db)^2, the 2 being
// TONEMAP_CIE_CHROMA_WEIGHT. The table holds each entry as q15 L, q15 2a and q15 2b, so the weight
// is folded in, and the kernel doubles the point's A and B with saturation. Each difference is a
// saturated 16 bit subtraction. Its square is exact in 32 bits. The three squares add in 32 bits,
// held at 2^31 - 1. The distance is in q15 counts squared, so 2^30 is a weighted squared distance of 1.
//
// Ranking. The search ranks entries by the distance rounded down to a multiple of 128 counts
// squared (1.2e-7), then by entry index. So ties go to the lower entry index, and two entries change
// places only when their distances differ by less than 128 counts squared. Two neutral entries 16
// counts apart in L rank right unless the point is within 4 counts of their midpoint.
//
// Range. The ranking above is exact while no difference saturates. A difference saturates only when
// the point is more than 1.0 from the entry in that one weighted component, and that entry then keeps
// a distance of at least 0.9999. So any best or next entry nearer than that is found exactly. The
// point's L, 2A and 2B are held at or below 1.0 plus the lowest entry L, 2a and 2b, so a difference
// never reaches -1.0. With this table that holds A at 0.498 and B at 0.489, and 2A and 2B saturate
// below -0.5. Both move the point. Oklab a and b of sRGB colours stay inside 0.33.
//
// The kernel reads the table of main/tone_tables.inc, built on the host from k_tonemap
// (host_tests/tone_tables). The build refuses a table with an entry's L, 2a or 2b outside -1 to 1.
#pragma once

#include <cstdint>

#include "tone_fx.h"

// The entry nearest to the point (L, A, B) in the weighted distance, and the second nearest, as
// indices into k_tonemap. L, A and B are Oklab L, a and b in q15, without the chroma weight. best and
// next always differ.
void tone_fx_search(q15 L, q15 A, q15 B, uint8_t* best, uint8_t* next);
