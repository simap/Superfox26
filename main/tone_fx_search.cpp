// Nearest and second nearest tone entry, fixed point. See tone_fx_search.h.

#include "tone_fx_search.h"

#include "tone_fx_search_internal.h"
#include "tone_tables.h"

namespace
{
using namespace tone_fx_search_internal;

// Read per coloured pixel: internal RAM, initialized data.
TONE_TABLES_RAM TONE_FX_ALIGNED table s_tab = tone_tables::search;
// The highest L, 2A and 2B of a point are 32767 plus the lowest entry L, 2a and 2b when that is
// negative. The search subtracts the point from the entry, so a point held there keeps every
// difference at -32767 or above. With the current table only A and B are held, at 0.498 and 0.489.
const int s_pl_max = tone_tables::search_max[0], s_pa_max = tone_tables::search_max[1],
          s_pb_max = tone_tables::search_max[2];

// The point as the search uses it. L, 2A and 2B are saturated to 16 bits and held at the maxima above.
inline void point(q15 L, q15 A, q15 B, int* pl, int* pa, int* pb)
{
    const int l = L, a = sat16(2 * A), b = sat16(2 * B);
    *pl         = l < s_pl_max ? l : s_pl_max;
    *pa         = a < s_pa_max ? a : s_pa_max;
    *pb         = b < s_pb_max ? b : s_pb_max;
}
} // namespace

const tone_fx_search_internal::table& tone_fx_search_table()
{
    return s_tab;
}

void tone_fx_search_point(q15 L, q15 A, q15 B, int* pl, int* pa, int* pb)
{
    point(L, A, B, pl, pa, pb);
}

#if TONE_FX_PIE
// One block of 8 entries. q0 and q1 hold the running first and second minimum of the keys per 32 bit
// lane. On entry q6 and q7 hold the previous block's two key vectors ordered per lane (lo and hi),
// SAR is 0, and tp points at the block. The block merges lo and hi into q0 and q1 between its own
// first instructions, and leaves its own lo and hi in q6 and q7.
//
// The key of an entry. Each difference is entry minus point, saturated to 16 bits by ee.vsubs.s16.
// The point's maxima keep it at -32767 or above, so the result does not depend on whether the unit
// saturates a negative difference to -32768 (the TRM) or to -32767 (QEMU). A difference above 32767
// saturates to 32767 in both. The square of a difference is at most 2^30, formed exactly from its
// two 16 bit halves. ee.vadds.s32 saturates (dl^2 + da^2) and then adds db^2 with saturation, which
// is the sum held at 2^31 - 1 since every term is positive. The key is that sum rounded down to a
// multiple of 128 with the entry index in the low 7 bits, so keys are distinct and order as
// (distance, entry index).
//
// A square is two ee.vmul.s16 of the difference by itself. SAR 0 keeps bits 15 to 0 of the product,
// SAR 16 bits 31 to 16, and ee.vzip.16 of the two puts them side by side as 32 bit lanes, entries 0 to
// 3 in the first register and 4 to 7 in the second.
//
// Per lane j, the keys of entries j and 4 + j of a block are ordered into lo and hi, then merged
// into the running first and second minimum m1 and m2. The result is the two smallest keys of the
// lane whatever the order.
//
// 37 instructions: 9 loads and broadcasts, 3 subtractions, 6 multiplies, 2 ssai, 3 zips, 4 additions,
// 2 and, 2 or, 6 min and max. The point and KEY_MASK are loaded again in every block, which leaves
// q2 to q7 to the block and puts at least one instruction between every result and its first reader.
//
// Expected time. The core issues one instruction per cycle. A vector load's result is ready the
// second cycle after it issues, and a register can be written the cycle after its last read. If every
// other vector result is ready by the second cycle as well, no instruction in the asm block waits.
// That is 492 instructions in 493 cycles, about 550 cycles per search with the 53 scalar
// instructions around it. If an ee.vmul.s16 cannot read a SAR set the cycle before, each block waits
// one cycle more.
#define SEARCH_BLOCK                                                                                   \
    "ee.vld.128.ip  q2, %[tp], 16   \n" /* 2a */                                                       \
    "ee.vldbc.16    q3, %[pa]       \n" /* 2A in every lane */                                         \
    "ee.vld.128.ip  q4, %[tp], 16   \n" /* L */                                                        \
    "ee.vldbc.16    q5, %[pl]       \n" /* point L in every lane */                                    \
    "ee.vmin.s32    q1, q1, q7      \n" /* m2 = min(m2, hi) */                                         \
    "ee.vmax.s32    q7, q0, q6      \n" /* t = max(m1, lo) */                                          \
    "ee.vsubs.s16   q2, q2, q3      \n" /* da = 2a - 2A, saturated */                                  \
    "ee.vsubs.s16   q4, q4, q5      \n" /* dl = L - point L */                                         \
    "ee.vmin.s32    q0, q0, q6      \n" /* m1 = min(m1, lo) */                                         \
    "ee.vld.128.ip  q3, %[tp], 16   \n" /* 2b */                                                       \
    "ee.vldbc.16    q5, %[pb]       \n" /* 2B in every lane */                                         \
    "ee.vmul.s16    q6, q2, q2      \n" /* da^2 bits 15..0 */                                          \
    "ee.vsubs.s16   q3, q3, q5      \n" /* db = 2b - 2B */                                             \
    "ee.vmin.s32    q1, q1, q7      \n" /* m2 = min(m2, t) */                                          \
    "ee.vmul.s16    q7, q4, q4      \n" /* dl^2 bits 15..0 */                                          \
    "ee.vmul.s16    q5, q3, q3      \n" /* db^2 bits 15..0 */                                          \
    "ssai           16              \n"                                                                \
    "ee.vmul.s16    q2, q2, q2      \n" /* da^2 bits 31..16 */                                         \
    "ee.vmul.s16    q4, q4, q4      \n" /* dl^2 bits 31..16 */                                         \
    "ee.vmul.s16    q3, q3, q3      \n" /* db^2 bits 31..16 */                                         \
    "ee.vzip.16     q6, q2          \n" /* da^2, 32 bit: q6 entries 0..3, q2 entries 4..7 */          \
    "ee.vzip.16     q7, q4          \n" /* dl^2 */                                                     \
    "ee.vzip.16     q5, q3          \n" /* db^2 */                                                     \
    "ee.vadds.s32   q6, q6, q7      \n"                                                                \
    "ee.vadds.s32   q2, q2, q4      \n"                                                                \
    "ee.vld.128.ip  q7, %[pm], 0    \n" /* KEY_MASK */                                                 \
    "ee.vadds.s32   q6, q6, q5      \n" /* distance of entries 0..3, held at 2^31 - 1 */               \
    "ee.vld.128.ip  q4, %[tp], 16   \n" /* tags of entries 0..3 */                                     \
    "ee.vadds.s32   q2, q2, q3      \n" /* distance of entries 4..7 */                                 \
    "ee.vld.128.ip  q5, %[tp], 16   \n" /* tags of entries 4..7 */                                     \
    "ee.andq        q6, q6, q7      \n"                                                                \
    "ee.andq        q2, q2, q7      \n"                                                                \
    "ee.orq         q6, q6, q4      \n" /* keys of entries 0..3 */                                     \
    "ee.orq         q2, q2, q5      \n" /* keys of entries 4..7 */                                     \
    "ssai           0               \n"                                                                \
    "ee.vmax.s32    q7, q6, q2      \n" /* hi */                                                       \
    "ee.vmin.s32    q6, q6, q2      \n" /* lo */

void tone_fx_search(q15 L, q15 A, q15 B, uint8_t* best, uint8_t* next)
{
    int pl, pa, pb;
    point(L, A, B, &pl, &pa, &pb);
    const q15               pt[3] = {static_cast<q15>(pl), static_cast<q15>(pa), static_cast<q15>(pb)};
    TONE_FX_ALIGNED int32_t m[8];
    const block*            tp  = s_tab.blk;
    const int32_t*          pn  = s_tab.none;
    const int32_t*          pm  = s_tab.mask;
    int32_t*                out = m;
    // KEY_NONE in m1, m2, lo and hi makes the first block's merge change nothing.
    __asm__ volatile("ee.vld.128.ip  q0, %[pn], 0    \n"
                     "ee.orq         q1, q0, q0      \n"
                     "ee.orq         q6, q0, q0      \n"
                     "ee.orq         q7, q0, q0      \n"
                     "ssai           0               \n"
                     ".rept %[nb]                    \n" SEARCH_BLOCK ".endr                          \n"
                     "ee.vmin.s32    q1, q1, q7      \n" /* the last block's merge */
                     "ee.vmax.s32    q7, q0, q6      \n"
                     "ee.vmin.s32    q0, q0, q6      \n"
                     "ee.vmin.s32    q1, q1, q7      \n"
                     "ee.vst.128.ip  q0, %[out], 16  \n"
                     "ee.vst.128.ip  q1, %[out], 16  \n"
                     : [tp] "+r"(tp), [pn] "+r"(pn), [pm] "+r"(pm), [out] "+r"(out)
                     : [pl] "r"(&pt[0]), [pa] "r"(&pt[1]), [pb] "r"(&pt[2]), [nb] "i"(N_BLOCK)
                     : "memory");
    pick(m, m + 4, best, next);
}
#endif
