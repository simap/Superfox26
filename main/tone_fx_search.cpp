// tone_fx_search() is the vector unit kernel and compiles only when TONE_FX_PIE is set, which is
// the ESP32-S3 target.
// tone_fx_search_table() and tone_fx_search_point() compile on every target, so the host test can
// read the same search table and apply the same point scaling the kernel uses.

#include "tone_fx_search.h"

#include "tone_fx_search_internal.h"
#include "tone_tables.h"

namespace
{
using namespace tone_fx_search_internal;

TONE_TABLES_RAM TONE_FX_ALIGNED table s_tab = tone_tables::search;
const int s_pl_max = tone_tables::search_max[0], s_pa_max = tone_tables::search_max[1],
          s_pb_max = tone_tables::search_max[2];

inline void point(q15 L, q15 A, q15 B, int* pl, int* pa, int* pb)
{
    const int l = L, a = sat16(2 * A), b = sat16(2 * B);
    *pl         = l < s_pl_max ? l : s_pl_max;
    *pa         = a < s_pa_max ? a : s_pa_max;
    *pb         = b < s_pb_max ? b : s_pb_max;
}
}

const tone_fx_search_internal::table& tone_fx_search_table()
{
    return s_tab;
}

void tone_fx_search_point(q15 L, q15 A, q15 B, int* pl, int* pa, int* pb)
{
    point(L, A, B, pl, pa, pb);
}

#if TONE_FX_PIE
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
    "ee.vmin.s32    q6, q6, q2      \n"

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
    __asm__ volatile("ee.vld.128.ip  q0, %[pn], 0    \n"
                     "ee.orq         q1, q0, q0      \n"
                     "ee.orq         q6, q0, q0      \n"
                     "ee.orq         q7, q0, q0      \n"
                     "ssai           0               \n"
                     ".rept %[nb]                    \n" SEARCH_BLOCK ".endr                          \n"
                     "ee.vmin.s32    q1, q1, q7      \n"
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
