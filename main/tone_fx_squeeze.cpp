// The kernel reads the coefficients of s_k in K_* enum order and rewinds to the start each pass, so
// s_k must hold all K_COUNT entries in that order. With the tone curve it reads the curve stream from
// K_CT8 to K_EHALF, steps back to K_PM0 and reads on to K_H1B, which leaves it at K_CT8 again.
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
static_assert(2 * (K_COUNT - K_PM0) <= 128, "the step back of the parameter pointer is one addi");

namespace
{
// The kernel without the tone curve. It reads the entries from K_ONE to K_H1B only.
inline void squeeze_flat(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co)
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
        : [kback] "i"(-2 * K_BASE_COUNT)
        : "memory");
}

// The kernel with the tone curve. L0 waits in q1 while the curve gives L1 in q2, with its scale
// steps at shift 0. The image edge at L0 and at L1 then run side by side on one set of loads from
// the curve stream, and the rest runs as in squeeze_flat.
inline void squeeze_curve(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co)
{
    const q15*     pl  = L;
    const q15*     pa  = A;
    const q15*     pb  = B;
    q15*           plo = Lo;
    q15*           pao = Ao;
    q15*           pbo = Bo;
    q15*           pco = Co;
    const int16_t* k   = s_k.data() + K_CT8;
    int            n8  = (n + 7) >> 3;
    if(n8 <= 0) return;
    __asm__ volatile(
        "ssai            15                  \n"
        "ee.zero.q       q7                  \n"
        "j               2f                  \n"
        ".balign         4                   \n"
        "2:                                  \n"
        "ee.vld.128.ip   q1, %[pl], 16       \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CT8 */
        "ee.vmax.s16     q1, q1, q7          \n" /* L0 */
        "ssai            0                   \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n" /* CM8 */
        "ee.vcmp.lt.s16  q4, q1, q2          \n" /* m8 */
        "ee.vmul.s16     q0, q1, q3          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CT4 */
        "ee.andq         q0, q0, q4          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n" /* CM4 */
        "ee.vadds.s16    q0, q1, q0          \n"
        "ee.vcmp.lt.s16  q5, q0, q2          \n" /* m4 */
        "ee.vmul.s16     q2, q0, q3          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n" /* CT2 */
        "ee.andq         q2, q2, q5          \n"
        "ee.vadds.s16    q0, q0, q2          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CM2 */
        "ee.vcmp.lt.s16  q6, q0, q3          \n" /* m2 */
        "ee.vmul.s16     q3, q0, q2          \n"
        "ssai            15                  \n"
        "ee.andq         q3, q3, q6          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CF */
        "ee.vadds.s16    q0, q0, q3          \n" /* x, 8192 to 32767 */
        "ee.vcmp.gt.s16  q3, q1, q7          \n"
        "ee.andq         q3, q3, q2          \n" /* f */
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CD8 */
        "ee.vmul.s16     q2, q3, q2          \n"
        "ee.andq         q2, q2, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* CONE */
        "ee.vsubs.s16    q3, q3, q2          \n"
        "ee.vsubs.s16    q0, q0, q4          \n" /* u */
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* C4 */
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CD4 */
        "ee.vmul.s16     q4, q4, q0          \n"
        "ee.vmul.s16     q2, q3, q2          \n"
        "ee.andq         q2, q2, q5          \n"
        "ee.vldbc.16.ip  q5, %[k], 2         \n" /* C3 */
        "ee.vsubs.s16    q3, q3, q2          \n"
        "ee.vadds.s16    q4, q4, q5          \n"
        "ee.vldbc.16.ip  q2, %[k], 2         \n" /* CD2 */
        "ee.vmul.s16     q4, q4, q0          \n"
        "ee.vmul.s16     q2, q3, q2          \n"
        "ee.vldbc.16.ip  q5, %[k], 2         \n" /* C2 */
        "ee.andq         q2, q2, q6          \n"
        "ee.vadds.s16    q4, q4, q5          \n"
        "ee.vsubs.s16    q3, q3, q2          \n" /* f */
        "ee.vldbc.16.ip  q5, %[k], 2         \n" /* C1 */
        "ee.vmul.s16     q4, q4, q0          \n"
        "ee.vldbc.16.ip  q6, %[k], 2         \n" /* C0 */
        "ee.vadds.s16    q4, q4, q5          \n"
        "ee.vmul.s16     q4, q4, q0          \n"
        "ee.vadds.s16    q4, q4, q6          \n" /* p */
        "ee.vmul.s16     q4, q4, q3          \n"
        "ee.vsubs.s16    q2, q7, q4          \n" /* L1 */
        "ee.vldbc.16.ip  q3, %[k], 2         \n" /* EONE */
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* EXC */
        "ee.vsubs.s16    q0, q3, q1          \n"
        "ee.vsubs.s16    q3, q3, q2          \n"
        "ee.vldbc.16.ip  q5, %[k], 2         \n" /* EU3 */
        "ee.vmin.s16     q0, q0, q4          \n" /* x of L0 */
        "ee.vmin.s16     q3, q3, q4          \n" /* x of L1 */
        "ee.vmul.s16     q6, q3, q5          \n"
        "ee.vmul.s16     q5, q0, q5          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* EU2 */
        "ee.vadds.s16    q5, q5, q4          \n"
        "ee.vadds.s16    q6, q6, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* EU1 */
        "ee.vmul.s16     q5, q0, q5          \n"
        "ee.vmul.s16     q6, q3, q6          \n"
        "ee.vadds.s16    q5, q5, q4          \n"
        "ee.vadds.s16    q6, q6, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* ES */
        "ee.vmul.s16     q5, q0, q5          \n"
        "ee.vmul.s16     q6, q3, q6          \n"
        "ee.vmul.s16     q0, q1, q4          \n"
        "ee.vmul.s16     q3, q2, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* ELC */
        "ee.vmin.s16     q5, q5, q0          \n"
        "ee.vmin.s16     q6, q6, q3          \n"
        "ee.vmin.s16     q0, q1, q4          \n"
        "ee.vmin.s16     q3, q2, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* ECM */
        "ee.vmul.s16     q0, q0, q4          \n"
        "ee.vmul.s16     q3, q3, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* ECB */
        "ee.vadds.s16    q0, q0, q4          \n"
        "ee.vadds.s16    q3, q3, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* EG */
        "ee.vmin.s16     q5, q5, q0          \n" /* e0 */
        "ee.vmin.s16     q6, q6, q3          \n" /* e1 */
        "ee.vmul.s16     q6, q6, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* ELB */
        "ee.vadds.s16    q0, q2, q6          \n"
        "ee.vld.128.ip   q2, %[pa], 16       \n"
        "ee.vmax.s16     q0, q0, q7          \n" /* L2 */
        "ee.vldbc.16.ip  q3, %[k], 2         \n" /* EH0 */
        "ee.vmax.s16     q4, q0, q4          \n"
        "ee.vmul.s16     q2, q2, q3          \n"
        "ee.vst.128.ip   q4, %[plo], 16      \n"
        "ee.vld.128.ip   q3, %[pb], 16       \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* EH1 */
        "ee.vmul.s16     q3, q3, q4          \n"
        "ee.vldbc.16.ip  q4, %[k], 2         \n" /* ELSB */
        "ee.vadds.s16    q2, q2, q3          \n"
        "ee.vldbc.16.ip  q3, %[k], 2         \n" /* EHALF */
        "ee.vmax.s16     q2, q2, q7          \n" /* c */
        "ee.vmax.s16     q6, q5, q4          \n" /* e */
        "addi            %[k], %[k], %[kback]\n"
        "ee.vmin.s16     q1, q2, q6          \n"
        "ee.vsubs.s16    q4, q6, q4          \n"
        "ee.vmul.s16     q5, q4, q3          \n" /* hm */
        "ee.vcmp.gt.s16  q2, q1, q5          \n"
        "ee.vadds.s16    q1, q1, q1          \n"
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
        : [kback] "i"(-2 * (K_COUNT - K_PM0))
        : "memory");
}
} // namespace

void tone_fx_squeeze(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co)
{
    if constexpr(CURVE) squeeze_curve(L, A, B, n, Lo, Ao, Bo, Co);
    else squeeze_flat(L, A, B, n, Lo, Ao, Bo, Co);
}

#if TONES_BENCH
// The kernel without the tone curve on the same parameters, for the benchmark's cost of the curve.
void tone_fx_squeeze_flat(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co)
{
    squeeze_flat(L, A, B, n, Lo, Ao, Bo, Co);
}
#endif

#endif
