// The kernel reads the coefficients of s_k in K_* enum order and rewinds to the start each pass, so
// s_k must hold all K_COUNT entries in that order.
#include "tone_fx_squeeze.h"

#include <cstdint>

#include "tone_fx_squeeze_internal.h"
#include "tone_tables.h"

namespace
{
using namespace tone_fx_squeeze_internal;

TONE_TABLES_RAM alignas(16) std::array<int16_t, K_COUNT> s_k = tone_tables::squeeze_k;
}

const int16_t* tone_fx_squeeze_params()
{
    return s_k.data();
}

#if TONE_FX_PIE

#define SQZ_STR2(x) #x
#define SQZ_STR(x) SQZ_STR2(x)

#define SQZ_BIT                                                                                                        \
    "ee.vcmp.gt.s16  q2, q1, q5          \n" /* bit = r > hm, that is 2r >= e */                                       \
    "ee.vadds.s16    q1, q1, q1          \n" /* r = 2r */                                                              \
    "ee.andq         q3, q2, q6          \n" /* e where the bit is 1 */                                                \
    "ee.vadds.s16    q4, q4, q4          \n" /* q = 2q */                                                              \
    "ee.vsubs.s16    q1, q1, q3          \n" /* r -= e where the bit is 1 */                                           \
    "ee.vsubs.s16    q4, q4, q2          \n"

static_assert(SQZ_DIV_BITS == 9, "the vector kernel unrolls 9 quotient bits");

void tone_fx_squeeze(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co)
{
    const q15* pl  = L;
    const q15* pa  = A;
    const q15* pb  = B;
    q15*       plo = Lo;
    q15*       pao = Ao;
    q15*       pbo = Bo;
    q15*       pco = Co;
    const int16_t* k   = s_k.data();
    int            n8  = (n + 7) >> 3;
    if(n8 <= 0) return;
    __asm__ volatile(
        "ssai            15                  \n"
        "ee.zero.q       q7                  \n"
        "j               2f                  \n"
        ".balign         4                   \n"
        "2:                                  \n"
        "ee.vld.128.ip   q0, %[pl], 16       \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n"
        "ee.vld.128.ip   q1, %[pa], 16       \n"
        "ee.vldbc.16.ip  q6, %[k], 2         \n"
        "ee.vmax.s16     q0, q0, q7          \n"
        "ee.vldbc.16.ip  q5, %[k], 2         \n"
        "ee.vsubs.s16    q4, q4, q0          \n"
        "ee.vld.128.ip   q2, %[pb], 16       \n"
        "ee.vmin.s16     q4, q4, q6          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n"
        "ee.vmul.s16     q5, q4, q5          \n"
        "ee.vldbc.16.ip  q6, %[k], 2         \n"
        "ee.vmul.s16     q1, q1, q3          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n"
        "ee.vmul.s16     q2, q2, q6          \n"
        "ee.vadds.s16    q5, q5, q3          \n"
        "ee.vldbc.16.ip  q6, %[k], 2         \n"
        "ee.vmul.s16     q5, q4, q5          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n"
        "ee.vmul.s16     q6, q0, q6          \n"
        "ee.vadds.s16    q1, q1, q2          \n"
        "ee.vadds.s16    q5, q5, q3          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n"
        "ee.vmul.s16     q5, q4, q5          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n"
        "ee.vmax.s16     q1, q1, q7          \n"
        "ee.vmin.s16     q2, q0, q2          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n"
        "ee.vmul.s16     q2, q2, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n"
        "ee.vmin.s16     q6, q6, q5          \n"
        "ee.vldbc.16.ip  q5, %[k], 2         \n"
        "ee.vadds.s16    q2, q2, q3          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n"
        "ee.vmin.s16     q6, q6, q2          \n"
        "ee.vmul.s16     q3, q6, q3          \n"
        "ee.vmax.s16     q6, q6, q4          \n"
        "ee.vmin.s16     q1, q1, q6          \n"
        "ee.vsubs.s16    q4, q6, q4          \n"
        "ee.vmul.s16     q5, q4, q5          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n"
        "ee.vadds.s16    q0, q0, q3          \n"
        "ee.vcmp.gt.s16  q2, q1, q5          \n"
        "ee.vmax.s16     q0, q0, q7          \n"
        "ee.vmax.s16     q4, q0, q4          \n"
        "ee.vadds.s16    q1, q1, q1          \n"
        "ee.vst.128.ip   q4, %[plo], 16      \n"
        "ee.andq         q3, q2, q6          \n"
        "ee.vsubs.s16    q4, q7, q2          \n"
        "ee.vsubs.s16    q1, q1, q3          \n"
        SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT
        "ee.vldbc.16.ip  q3, %[k], 2         \n"
        "ee.vcmp.eq.s16  q2, q1, q6          \n"
        "ee.vldbc.16.ip  q5, %[k], 2         \n"
        "ee.vmul.s16     q3, q0, q3          \n"
        "ee.vldbc.16.ip  q1, %[k], 2         \n"
        "ee.vmul.s16     q5, q0, q5          \n"
        "ee.vldbc.16.ip  q6, %[k], 2         \n"
        "ee.vmul.s16     q1, q0, q1          \n"
        "ee.vsubs.s16    q4, q4, q2          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n"
        "ee.vmul.s16     q6, q0, q6          \n"
        "ee.vldbc.16.ip  q0, %[k], 2         \n"
        "ee.vadds.s16    q5, q5, q2          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n"
        "ee.vadds.s16    q1, q1, q0          \n"
        "ee.vmin.s16     q3, q3, q5          \n"
        "ee.vadds.s16    q6, q6, q2          \n"
        "ssai            " SQZ_STR(SQZ_DIV_BITS) "\n"
        "ee.vldbc.16.ip  q0, %[k], 2         \n"
        "ee.vmin.s16     q1, q1, q6          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n"
        "ee.vmin.s16     q3, q3, q1          \n"
        "ee.vmul.s16     q3, q4, q3          \n"
        "addi            %[k], %[k], %[kback]\n"
        "addi            %[n8], %[n8], -1    \n"
        "ssai            15                  \n"
        "ee.vmul.s16     q0, q3, q0          \n"
        "ee.vst.128.ip   q3, %[pco], 16      \n"
        "ee.vmul.s16     q2, q3, q2          \n"
        "ee.vst.128.ip   q0, %[pao], 16      \n"
        "ee.vst.128.ip   q2, %[pbo], 16      \n"
        "bnez            %[n8], 2b           \n"
        : [pl] "+r"(pl), [pa] "+r"(pa), [pb] "+r"(pb), [plo] "+r"(plo), [pao] "+r"(pao), [pbo] "+r"(pbo),
          [pco] "+r"(pco), [k] "+r"(k), [n8] "+r"(n8)
        : [kback] "i"(-2 * K_COUNT)
        : "memory");
}

#endif
