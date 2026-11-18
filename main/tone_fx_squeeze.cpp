// The squeeze of tonemap_squeeze.h in fixed point. tone_fx_squeeze.h has the interface.
//
// With TONEMAP_GAMMA 1 the float steps reduce to these, per colour:
//   L0  = max(L, 0)
//   c   = max(a * hue0 + b * hue1, 0), the chroma along the hue of sRGB red
//   e0  = img(L0), the image edge, the most chroma of an sRGB colour at L0 on that hue
//   e   = max(e0, 1), the divisor
//   cm  = min(c, e)
//   L2  = max(L0 + G * e0, 0), G = TONEMAP_COMPRESS * (cusp_l - red_l) / red_c
//   c2  = cm / e * pan(L2), pan the panel edge
// The move of L reads the edge and not the colour's chroma, so every colour of one lightness moves
// alike. e0 is 0 at white and at black, so both stay. L0 + G * e0 is 0 or above in real numbers.
// The floor at 0 catches the -1 the rounding of the product gives near black, where the panel edge
// would go below 0.
//   out L2 held at or above the black entry's L, c2 * hue0, c2 * hue1, and c2.
// The float code moves L by TONEMAP_COMPRESS * (flat - L) with flat = max(L - wr * red_l, 0) +
// wr * cusp_l and wr = img(L) / red_c. The image edge lies at or below the ray
// from black through sRGB red, img(L) <= red_c / red_l * L, so wr * red_l <= L, the clamp at 0
// never acts and flat - L = img(L) * (cusp_l - red_l) / red_c. The float result c2 / c * (a, b) is
// c2 * (hue0, hue1), since the float code set a and b to c * hue0 and c * hue1 the line before.
//
// Edge curves. A vector lane has no load by per lane index, so neither edge is read from the 65
// step tables of tonemap_squeeze_begin(). Both are evaluated in lanes from parameters fitted on the
// host to those tables (host_tests/tone_tables, into main/tone_tables.inc), as the tables
// interpolate them (squeeze_at()):
//   img(L) = min(S * L, chord(min(L, LC)), up(min(1 - L, XC)))
//     S * L   the ray from black through sRGB red. The table is on it up to the step before red.
//     chord   the table's straight line across the step that holds red, from LC - 1/64 to LC.
//             Its argument is held at LC, so right of the step it stays at the table value at LC,
//             above the upper branch.
//     up      the upper branch, red to white, a cubic x * (U1 + x * (U2 + x * U3)) in x = 1 - L,
//             fitted to the table from LC to 1. x is held at XC, the left end of the step that
//             holds red, so left of the step the cubic stays above the ray and the chord.
//   pan(L) = min over the lines j of PM_j * L + PB_j
//     The panel table is the upper hull of the entries at each step. Interpolated between steps,
//     it is concave, so it is the minimum of its step lines. Steps on one line merge into one.
//     Four lines are kept, the ones that miss the table least. The fit raises an intercept by 1
//     where the rounding takes the minimum below 0 for an L from 0 to 1, so pan(L) >= 0 there.
// main/tone_tables.inc records how far each fitted edge lies from its table (squeeze_fit_img and
// squeeze_fit_pan).
//
// Divide. share = cm / e has a divisor per lane. e runs from 0 at black and white to red_c, so a
// reciprocal in 16 bits would need a shift per lane to normalize, which the vector unit lacks.
// The share is a restoring division instead, DIV_BITS bits from r = cm, q = 0. Each bit is a
// compare, a doubling, a masked subtract and the bit added to the quotient q. The compare is
// r > hm with hm = floor((e - 1) / 2), which for integers is 2r >= e, so it reads r before the
// doubling and the dependent chain per bit is three operations (compare, and, subtract). r < e
// after every bit when cm < e. When cm == e every bit is 1 and r stays e, so q gets 1 more where
// r == e at the end. q = floor(cm / e * 2^DIV_BITS), 0 to 2^DIV_BITS, exact for every e, and the
// error of c2 is at most pan * 2^-DIV_BITS. A per lane shift is not needed and the cost is the
// same for every lane. The divide by red_c of the float code is folded into G.
//
// Arithmetic. Every step is an operation of the vector unit, and the stand in of the host tests runs
// the same operations element by element (tone_fx_squeeze_internal.h holds the lane steps that the
// fit shares with it):
//   mul(x, y, s)  the 32 bit product shifted right by s, arithmetic, rounding toward minus
//                 infinity, then the low 16 bits (ee.vmul.s16 with SAR = s). No product here
//                 leaves 16 bits.
//   adds, subs    16 bit sum and difference saturated to -32768..32767 (ee.vadds.s16,
//                 ee.vsubs.s16)
//   min, max, gt, eq, and  (ee.vmin.s16, ee.vmax.s16, ee.vcmp.gt.s16, ee.vcmp.eq.s16, ee.andq)
// Products of two q15 values use s = 15. The quotient times the panel edge uses s = DIV_BITS.
#include "tone_fx_squeeze.h"

#include <cstdint>

#include "tone_fx_squeeze_internal.h"
#include "tone_tables.h"

namespace
{
using namespace tone_fx_squeeze_internal;

// Read per chunk: internal RAM, initialized data.
TONE_TABLES_RAM alignas(16) std::array<int16_t, K_COUNT> s_k = tone_tables::squeeze_k;
} // namespace

const int16_t* tone_fx_squeeze_params()
{
    return s_k.data();
}

#if TONE_FX_PIE

#define SQZ_STR2(x) #x
#define SQZ_STR(x) SQZ_STR2(x)

// One quotient bit. q1 r, q5 hm, q6 e, q4 q, q2 the bit as a mask, q3 e where the bit is 1.
#define SQZ_BIT                                                                                                        \
    "ee.vcmp.gt.s16  q2, q1, q5          \n" /* bit = r > hm, that is 2r >= e */                                       \
    "ee.vadds.s16    q1, q1, q1          \n" /* r = 2r */                                                              \
    "ee.andq         q3, q2, q6          \n" /* e where the bit is 1 */                                                \
    "ee.vadds.s16    q4, q4, q4          \n" /* q = 2q */                                                              \
    "ee.vsubs.s16    q1, q1, q3          \n" /* r -= e where the bit is 1 */                                           \
    "ee.vsubs.s16    q4, q4, q2          \n" /* q += bit */

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
    // Schedule. The core issues one instruction per cycle, and a vector load's result is ready for
    // the second instruction after the load. The latencies of the other vector operations are not
    // known for this chip. Where the 8 q registers allow, the order keeps a vmul 3 instructions or
    // more from its first consumer, and other vector results and constant loads 1 or more. The
    // divide's dependent chain is 3 of the 6 instructions of each bit. The loop is 130 instructions
    // per 8 colours. The expected time is 132 cycles per 8 colours, 16.5 per colour, with results
    // ready for the next instruction and vmul results within 2. With vector add, compare and min at
    // 2 cycles, vmul at 4, an address step at 2 and an SAR write at 2 before its vmul, it is 142
    // cycles, 17.8 per colour. Both include a taken branch bubble of 2 to 3 cycles. The loop head
    // is 4 byte aligned for that branch. The order assumes a vmul reads SAR when it issues, and
    // keeps 2 instructions between every ssai and the vmul before it.
    // Each input is loaded before any output of its group is stored, so in place use works.
    __asm__ volatile(
        "ssai            15                  \n"
        "ee.zero.q       q7                  \n" // 0 for the whole loop
        "j               2f                  \n"
        ".balign         4                   \n"
        "2:                                  \n"
        "ee.vld.128.ip   q0, %[pl], 16       \n" // L
        "ee.vldbc.16.ip  q4, %[k], 2         \n" // ONE
        "ee.vld.128.ip   q1, %[pa], 16       \n" // a
        "ee.vldbc.16.ip  q6, %[k], 2         \n" // XC
        "ee.vmax.s16     q0, q0, q7          \n" // L0 = max(L, 0)
        "ee.vldbc.16.ip  q5, %[k], 2         \n" // U3
        "ee.vsubs.s16    q4, q4, q0          \n" // 1 - L0
        "ee.vld.128.ip   q2, %[pb], 16       \n" // b
        "ee.vmin.s16     q4, q4, q6          \n" // x = min(1 - L0, XC)
        "ee.vldbc.16.ip  q3, %[k], 2         \n" // H0
        "ee.vmul.s16     q5, q4, q5          \n" // x * U3
        "ee.vldbc.16.ip  q6, %[k], 2         \n" // H1
        "ee.vmul.s16     q1, q1, q3          \n" // a * hue0
        "ee.vldbc.16.ip  q3, %[k], 2         \n" // U2
        "ee.vmul.s16     q2, q2, q6          \n" // b * hue1
        "ee.vadds.s16    q5, q5, q3          \n" // t = x * U3 + U2
        "ee.vldbc.16.ip  q6, %[k], 2         \n" // S
        "ee.vmul.s16     q5, q4, q5          \n" // x * t
        "ee.vldbc.16.ip  q3, %[k], 2         \n" // U1
        "ee.vmul.s16     q6, q0, q6          \n" // ray S * L0
        "ee.vadds.s16    q1, q1, q2          \n" // c
        "ee.vadds.s16    q5, q5, q3          \n" // t = x * t + U1
        "ee.vldbc.16.ip  q2, %[k], 2         \n" // LC
        "ee.vmul.s16     q5, q4, q5          \n" // upper branch x * t
        "ee.vldbc.16.ip  q4, %[k], 2         \n" // CM
        "ee.vmax.s16     q1, q1, q7          \n" // c = max(c, 0)
        "ee.vmin.s16     q2, q0, q2          \n" // min(L0, LC)
        "ee.vldbc.16.ip  q3, %[k], 2         \n" // CB
        "ee.vmul.s16     q2, q2, q4          \n" // chord slope part
        "ee.vldbc.16.ip  q4, %[k], 2         \n" // 1
        "ee.vmin.s16     q6, q6, q5          \n" // e = min(ray, upper)
        "ee.vldbc.16.ip  q5, %[k], 2         \n" // HALF
        "ee.vadds.s16    q2, q2, q3          \n" // chord
        "ee.vldbc.16.ip  q3, %[k], 2         \n" // G
        "ee.vmin.s16     q6, q6, q2          \n" // e0 = min(e, chord)
        "ee.vmul.s16     q3, q6, q3          \n" // e0 * G
        "ee.vmax.s16     q6, q6, q4          \n" // e = max(e0, 1)
        "ee.vmin.s16     q1, q1, q6          \n" // cm = min(c, e), the divide's r from here
        "ee.vsubs.s16    q4, q6, q4          \n" // e - 1
        "ee.vmul.s16     q5, q4, q5          \n" // hm = floor((e - 1) / 2)
        "ee.vldbc.16.ip  q4, %[k], 2         \n" // LB
        "ee.vadds.s16    q0, q0, q3          \n" // L2 = L0 + e0 * G
        "ee.vcmp.gt.s16  q2, q1, q5          \n" // first bit = r > hm
        "ee.vmax.s16     q0, q0, q7          \n" // L2 = max(L2, 0)
        "ee.vmax.s16     q4, q0, q4          \n" // Lo = max(L2, LB)
        "ee.vadds.s16    q1, q1, q1          \n" // r = 2r
        "ee.vst.128.ip   q4, %[plo], 16      \n" // Lo
        "ee.andq         q3, q2, q6          \n" // e where the bit is 1
        "ee.vsubs.s16    q4, q7, q2          \n" // q = bit
        "ee.vsubs.s16    q1, q1, q3          \n" // r -= e where the bit is 1
        SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT SQZ_BIT
        "ee.vldbc.16.ip  q3, %[k], 2         \n" // PM0
        "ee.vcmp.eq.s16  q2, q1, q6          \n" // r == e where cm == e
        "ee.vldbc.16.ip  q5, %[k], 2         \n" // PM1
        "ee.vmul.s16     q3, q0, q3          \n" // pan = line 0
        "ee.vldbc.16.ip  q1, %[k], 2         \n" // PM2
        "ee.vmul.s16     q5, q0, q5          \n" // line 1 slope part
        "ee.vldbc.16.ip  q6, %[k], 2         \n" // PM3
        "ee.vmul.s16     q1, q0, q1          \n" // line 2 slope part
        "ee.vsubs.s16    q4, q4, q2          \n" // q += 1 where cm == e
        "ee.vldbc.16.ip  q2, %[k], 2         \n" // PB1
        "ee.vmul.s16     q6, q0, q6          \n" // line 3 slope part
        "ee.vldbc.16.ip  q0, %[k], 2         \n" // PB2
        "ee.vadds.s16    q5, q5, q2          \n" // line 1
        "ee.vldbc.16.ip  q2, %[k], 2         \n" // PB3
        "ee.vadds.s16    q1, q1, q0          \n" // line 2
        "ee.vmin.s16     q3, q3, q5          \n" // pan = min(line 0, line 1)
        "ee.vadds.s16    q6, q6, q2          \n" // line 3
        "ssai            " SQZ_STR(SQZ_DIV_BITS) "\n"
        "ee.vldbc.16.ip  q0, %[k], 2         \n" // H0
        "ee.vmin.s16     q1, q1, q6          \n" // min(line 2, line 3)
        "ee.vldbc.16.ip  q2, %[k], 2         \n" // H1
        "ee.vmin.s16     q3, q3, q1          \n" // pan
        "ee.vmul.s16     q3, q4, q3          \n" // c2 = q * pan >> DIV_BITS
        "addi            %[k], %[k], %[kback]\n" // back to the first constant
        "addi            %[n8], %[n8], -1    \n"
        "ssai            15                  \n"
        "ee.vmul.s16     q0, q3, q0          \n" // a = c2 * hue0
        "ee.vst.128.ip   q3, %[pco], 16      \n" // Co
        "ee.vmul.s16     q2, q3, q2          \n" // b = c2 * hue1
        "ee.vst.128.ip   q0, %[pao], 16      \n" // Ao
        "ee.vst.128.ip   q2, %[pbo], 16      \n" // Bo
        "bnez            %[n8], 2b           \n"
        : [pl] "+r"(pl), [pa] "+r"(pa), [pb] "+r"(pb), [plo] "+r"(plo), [pao] "+r"(pao), [pbo] "+r"(pbo),
          [pco] "+r"(pco), [k] "+r"(k), [n8] "+r"(n8)
        : [kback] "i"(-2 * K_COUNT)
        // GCC has no clobber name for SAR. Its own shift patterns set SAR in the same instruction
        // pair as the shift that reads it, so no value of the compiler lives in SAR across this
        // block.
        : "memory");
}

#endif // TONE_FX_PIE
