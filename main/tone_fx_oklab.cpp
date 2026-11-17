// Oklab fixed point kernels. See tone_fx_oklab.h.
//
// The stand in of the host tests models the vector unit lane by lane: the same operations in the same
// order with the same shifts, rounding and saturation, so that it gives the bits of these kernels.
// The constants below are in tone_fx_oklab_internal.h.
//
// Cube root core. A positive value v is written v = x'' * 2^-(3q + r) / 32768 with x'' in 16384 to
// 32767 (the real 1/2 to 1), q a whole number and r 0, 1 or 2. Then
//   cbrt(v) = 2^-q * F_r * p(t),  F_r = 2^(-r/3),  t = 4 * (x'' - 24576) + 2,
// and p(t) = cbrt(3/4 + t / 131072) on t in -32766 to 32766 is a degree 5 polynomial. The core
// keeps p as c0 + D / (8 * 32768): D = 8 * (p - c0) in q15, about -0.87 to 0.79, so its rounding
// costs 1/8 count. With S = 2^(14 - q) the core forms in the 40 bit accumulator
//   cbrt(v) * 2^29 = S * G_r + S * (D * F_r) / 8,  G_r = c0 * F_r * 32768,
// where both products are exact and c0 = 29559 / 32768 makes G_1 and G_2 lie within 0.007 of whole
// numbers. host_tests/tone_fx/oklab/fit_cbrt.cpp fits the constants.
//
// The q15 cube root finds x'' exactly: four steps each multiply x by 8 where it is below 4096, then
// x is multiplied by 1, 2 or 4.
//
// Linear to Oklab. LMS = M1 * rgb is exact in the accumulator (q15 coefficients times q15 inputs,
// 2^30 per unit). A dark channel needs its low bits: one count of error in an LMS of 1/8 moves its
// cube root by 1.3 counts, and the second matrix multiplies cube root errors by up to 4.9. So the
// accumulator is read out at every shift e from 15 down to 0 (each read shifts it by 1 more) and the
// value at shift 0 is doubled four more times for e = -1 to -4. The highest e whose 16 bit value is
// 16384 or more gives x'' = floor(acc / 2^e), exact to one unit of 1/2 to 1. The pick is a min over
// v XOR 0xC000, which maps 16384..32767 to -32768..-16385 and 0..16383 above them. A count of the
// reads below 16384 gives j = 15 - e = 3q + r. The cube roots leave the core in offset q16,
// w = cbrt * 65536 - 32768, so the second matrix sees them to a quarter count.
//
// Oklab to linear. l, m and s are exact in the accumulator and read out in offset q16, which holds
// them to 0 to 1. The cubes are exact products of 16 bit halves, rounded once to offset q16. The last
// matrix sums the cubes in the accumulator and rounds once.
#include "tone_fx_oklab.h"

#if TONE_FX_PIE

#include <cstddef>
#include <cstdint>

#include "tone_fx_oklab_internal.h"

// ---------------------------------------------------------------- kernels
//
// Each block walks a stream of constants with ee.vldbc.16.ip (2 bytes a step) or ee.vld.128.ip
// (16 bytes a step) in the order the block uses them. A stream edit must keep that order.
//
// The expected cycles at each block count one cycle per instruction, single issue, plus the stalls
// of this model: a vector load or l32i and its consumer back to back stall 1 cycle. The model also
// takes a 1 cycle stall where the next instruction reads the result of ee.vmul, of ee.srcmb, of the
// last ee.vmulas before an ee.srcmb, or SAR after wsr.sar, and none for the other vector operations
// and for one ee.vmulas after another. Those latencies are not measured. A taken branch costs 3.
// host_tests/tone_fx/oklab/sched_check.py walks a disassembly with this model. The order of each
// block keeps producer and consumer apart where the model would stall, and the stalls left are
// named at the block. The hardware bench gives the real count.
//
// No output of a block depends on where ee.vadds.s16 and ee.vsubs.s16 saturate below: QEMU
// saturates them at -32767 and the TRM gives -32768, and every value that reaches an output stays
// above that bound.

namespace
{
using namespace tone_fx_oklab_internal;

#define OK_STR2(x) #x
#define OK_STR(x) OK_STR2(x)

// The cube root core. In: q0 = d, q1 = mA, q2 = mB, q3 = S, %[pk] at the core block of a stream,
// SAR 15. Out: q0. Uses q0 to q7, the accumulator, %[t0] and %[t1]. 49 instructions. The products t2
// to t5 and the loads of the next constants fill the gaps of the accumulator chain. The read at
// shift 3 and the multiply-add after it, and the last multiply-add and the read after it, stay back
// to back: 2 stalls in the model.
#define OK_CORE(SOUT)                                                                                              \
    "ee.vldbc.16.ip    q4, %[pk], 2       \n" /* F0 */                                                            \
    "ee.vadds.s16      q0, q0, q0         \n" /* 2d */                                                            \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* F1 - F0 */                                                       \
    "ee.vcmp.eq.s16    q7, q7, q7         \n" /* -1 */                                                            \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* F2 - F1 */                                                       \
    "ee.vsubs.s16      q0, q0, q7         \n" /* 2d + 1 */                                                        \
    "ee.andq           q5, q5, q1         \n"                                                                     \
    "ee.andq           q6, q6, q2         \n"                                                                     \
    "ee.vadds.s16      q0, q0, q0         \n" /* t = 4d + 2 */                                                    \
    "ee.vadds.s16      q4, q4, q5         \n"                                                                     \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* G0 */                                                            \
    "ee.vadds.s16      q4, q4, q6         \n" /* F */                                                             \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* G1 - G0 */                                                       \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* G2 - G1 */                                                       \
    "ee.andq           q6, q6, q1         \n"                                                                     \
    "ee.andq           q7, q7, q2         \n"                                                                     \
    "ee.vmul.s16       q1, q0, q0         \n" /* t2 */                                                            \
    "ee.vadds.s16      q5, q5, q6         \n"                                                                     \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* C1 */                                                            \
    "ee.vadds.s16      q5, q5, q7         \n" /* G */                                                             \
    "ee.zero.qacc                         \n"                                                                     \
    "ee.vmul.s16       q2, q1, q0         \n" /* t3 */                                                            \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* C2 */                                                            \
    "ee.vmulas.s16.qacc q0, q6            \n"                                                                     \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* C3 */                                                            \
    "ee.vmulas.s16.qacc q1, q7            \n"                                                                     \
    "ee.vmul.s16       q1, q1, q1         \n" /* t4 */                                                            \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* C4 */                                                            \
    "ee.vmulas.s16.qacc q2, q6            \n"                                                                     \
    "ee.vmul.s16       q2, q1, q0         \n" /* t5 */                                                            \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* C5 */                                                            \
    "ee.vmulas.s16.qacc q1, q7            \n"                                                                     \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* KD a */                                                          \
    "ee.vmulas.s16.qacc q2, q6            \n"                                                                     \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* KD b */                                                          \
    "movi              %[t0], 15          \n"                                                                     \
    "ee.vmulas.s16.qacc q7, q6            \n"                                                                     \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* KO a */                                                          \
    "ee.srcmb.s16.qacc q0, %[t0], 0       \n" /* D */                                                             \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* KO b */                                                          \
    "ee.zero.qacc                         \n"                                                                     \
    "ee.vmul.s16       q1, q0, q4         \n" /* D * F */                                                         \
    "movi              %[t0], 3           \n"                                                                     \
    "ee.vmulas.s16.qacc q1, q3            \n" /* D * F * S */                                                     \
    "movi              %[t1], " OK_STR(SOUT) "\n"                                                                 \
    "ee.srcmb.s16.qacc q2, %[t0], 0       \n" /* >> 3, value unused */                                            \
    "ee.vmulas.s16.qacc q5, q3            \n" /* G * S */                                                         \
    "ee.vmulas.s16.qacc q6, q7            \n" /* KO */                                                            \
    "ee.srcmb.s16.qacc q0, %[t1], 0       \n"

// Level reads of the Oklab path. Registers: q0 the read v, q1 f = v XOR 0xC000, q2 y = min f,
// q3 m, q4 cnt, q5 S, q6 = 0xC000, q7 = -16385. A level macro enters with q1 = f of its level and
// q0 already read, issues NEXT (the read of the next level into q0), finishes its own level and
// leaves f of the next level in q1. Every consumer sits two instructions after its producer.
#define OK_SRC "ee.srcmb.s16.qacc q0, %[one], 0 \n"
#define OK_DBL "ee.vadds.s16      q0, q0, q0     \n"
#define OK_LV(NEXT)                                                                                                \
    NEXT "ee.vmin.s16       q2, q2, q1     \n"                                                                     \
         "ee.vcmp.gt.s16    q3, q1, q7     \n"                                                                     \
         "ee.xorq           q1, q0, q6     \n"                                                                     \
         "ee.vadds.s16      q4, q4, q3     \n"
// A bottom level of a group of three: cnt takes m twice off, S takes the next stream value where m.
#define OK_LV_BOTTOM(NEXT)                                                                                         \
    NEXT "ee.vmin.s16       q2, q2, q1     \n"                                                                     \
         "ee.vcmp.gt.s16    q3, q1, q7     \n"                                                                     \
         "ee.vldbc.16.ip    q1, %[pk], 2   \n"                                                                     \
         "ee.vsubs.s16      q4, q4, q3     \n"                                                                     \
         "ee.andq           q1, q1, q3     \n"                                                                     \
         "ee.vsubs.s16      q4, q4, q3     \n"                                                                     \
         "ee.vsubs.s16      q5, q5, q1     \n"                                                                     \
         "ee.xorq           q1, q0, q6     \n"
// e = -4: an ordinary level that also takes the last 256 off S where m.
#define OK_LV_ZERO(NEXT)                                                                                           \
    NEXT "ee.vmin.s16       q2, q2, q1     \n"                                                                     \
         "ee.vcmp.gt.s16    q3, q1, q7     \n"                                                                     \
         "ee.vldbc.16.ip    q1, %[pk], 2   \n"                                                                     \
         "ee.vadds.s16      q4, q4, q3     \n"                                                                     \
         "ee.andq           q1, q1, q3     \n"                                                                     \
         "ee.vsubs.s16      q5, q5, q1     \n"                                                                     \
         "ee.xorq           q1, q0, q6     \n"

// From the accumulator holding LMS * 2^30 to the core inputs, the core and the cube root in q0
// (offset q16). Enters with q6 = 0xC000, q7 = -16385 loaded, %[pk] after them, %[t0] = 0. The level
// reads stall nowhere in the model, the core twice.
#define OK_LMS_CBRT                                                                                                \
    "ee.srcmb.s16.qacc q0, %[t0], 0       \n" /* e = 0 */                                                         \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* S = 16384 */                                                     \
    "ee.xorq           q1, q0, q6         \n"                                                                     \
    OK_DBL                                    /* e = -1 */                                                        \
    "ee.orq            q2, q1, q1         \n" /* y */                                                             \
    "ee.vcmp.gt.s16    q4, q1, q7         \n" /* cnt */                                                           \
    "ee.xorq           q1, q0, q6         \n"                                                                     \
    OK_LV(OK_DBL)        /* finishes -1, reads -2 */                                                              \
    OK_LV_BOTTOM(OK_DBL) /* -2, reads -3 */                                                                       \
    OK_LV(OK_DBL)        /* -3, reads -4 */                                                                       \
    OK_LV_ZERO(OK_SRC)   /* -4, reads 1 */                                                                        \
    OK_LV_BOTTOM(OK_SRC) /* 1 */                                                                                  \
    OK_LV(OK_SRC)        /* 2 */                                                                                  \
    OK_LV(OK_SRC)        /* 3 */                                                                                  \
    OK_LV_BOTTOM(OK_SRC) /* 4 */                                                                                  \
    OK_LV(OK_SRC)        /* 5 */                                                                                  \
    OK_LV(OK_SRC)        /* 6 */                                                                                  \
    OK_LV_BOTTOM(OK_SRC) /* 7 */                                                                                  \
    OK_LV(OK_SRC)        /* 8 */                                                                                  \
    OK_LV(OK_SRC)        /* 9 */                                                                                  \
    OK_LV_BOTTOM(OK_SRC) /* 10 */                                                                                 \
    OK_LV(OK_SRC)        /* 11 */                                                                                 \
    OK_LV(OK_SRC)        /* 12 */                                                                                 \
    OK_LV_BOTTOM(OK_SRC) /* 13 */                                                                                 \
    OK_LV(OK_SRC)        /* 14, reads 15 */                                                                       \
    "ee.vmin.s16       q2, q2, q1         \n" /* 15 */                                                            \
    "ee.vcmp.gt.s16    q3, q1, q7         \n"                                                                     \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* 24576 */                                                         \
    "ee.zero.q         q7                 \n"                                                                     \
    "ee.vadds.s16      q4, q4, q3         \n" /* cnt = -r */                                                      \
    "ee.vcmp.eq.s16    q3, q3, q3         \n" /* -1 */                                                            \
    "ee.vadds.s16      q0, q2, q6         \n" /* d = y + 24576 */                                                 \
    "ee.vcmp.lt.s16    q1, q4, q7         \n" /* mA: r >= 1 */                                                    \
    "ee.vcmp.lt.s16    q2, q4, q3         \n" /* mB: r = 2 */                                                     \
    "ee.orq            q3, q5, q5         \n" /* S */                                                             \
    OK_CORE(13)

#define OK_CORE_STREAM(P)                                                                                          \
    CORE_F0, CORE_F1 - CORE_F0, CORE_F2 - CORE_F1, CORE_G0, CORE_G1 - CORE_G0, CORE_G2 - CORE_G1, P.c1, P.c2,     \
        P.c3, P.c4, P.c5, P.kd_a, P.kd_b, P.ko_a, P.ko_b

// The level stream: the two level constants, S = 16384, the S steps at e = -2, -4, 1, 4, 7, 10, 13,
// 24576, then the core block of the floor path.
#define OK_LMS_STREAM                                                                                              \
    static_cast<int16_t>(0xC000), -16385, 16384, 256, 256, 512, 1024, 2048, 4096, 8192, 24576,                    \
        OK_CORE_STREAM(PATH_FLOOR)

alignas(16) const int16_t k_cbrt_stream[] = {4096, 4095, 7, 1024, 16384, 8192, 24576, OK_CORE_STREAM(PATH_EXACT)};
alignas(16) const int16_t k_lms_stream[]  = {OK_LMS_STREAM};
// The first matrix by rows, then the second matrix in the order of the last stage of the array form:
// L with the rounding pair and -32768 for its offset 2^30, then b, then a.
alignas(16) const int16_t k_l2o_rows[] = {M1[0][0], M1[0][1], M1[0][2], M1[1][0], M1[1][1], M1[1][2], M1[2][0], M1[2][1],
                                         M1[2][2], M2_L[0],  M2_L[1],  M2_L[2],  128,      256,      -32768,   M2_B[0],
                                         M2_B[1],  M2_B[2],  M2_A_L[0], M2_A_L[1], M2_A_M[0], M2_A_M[2], M2_A_S};
// One pixel: the first matrix by columns in lanes 0 to 2, the second matrix by columns with the split
// parts as extra columns, -32768 in lane 0 (times -32768 the offset 2^30 of L), then the rounding
// pair and -32768 for broadcast loads.
alignas(16) const int16_t k_l2o1_cols[] = {
    M1[0][0], M1[1][0], M1[2][0], 0, 0, 0, 0, 0, M1[0][1], M1[1][1], M1[2][1], 0, 0, 0, 0, 0,
    M1[0][2], M1[1][2], M1[2][2], 0, 0, 0, 0, 0, M2_L[0], M2_A_L[0], M2_B[0], 0, 0, 0, 0, 0,
    0, M2_A_L[1], 0, 0, 0, 0, 0, 0, M2_L[1], M2_A_M[0], M2_B[1], 0, 0, 0, 0, 0,
    0, M2_A_M[1], 0, 0, 0, 0, 0, 0, 0, M2_A_M[2], 0, 0, 0, 0, 0, 0,
    M2_L[2], M2_A_S, M2_B[2], 0, 0, 0, 0, 0, -32768, 0, 0, 0, 0, 0, 0, 0,
    128, 256, -32768, 0, 0, 0, 0, 0};

// Oklab to linear, in the order of the block: the first 3 values of channel l, then per channel the
// row constants, the residue and cube constants and the first 3 values of the next stage, then the
// last matrix.
#define OK_O2L_CUBE static_cast<int16_t>(0x8000), -32768, -16384, 32767
alignas(16) const int16_t k_o2l_stream[] = {
    16384, -32640, 16448,
    M3_LA, M3_LB, -32768, -16384, OK_O2L_CUBE, -32640, 16448, 16384,
    M3_MA, M3_MB, -32768, -16384, OK_O2L_CUBE, -32640, 16448, 16384,
    M3_SA, M3_SB0, M3_SB1, -32768, -16384, M3_SB0, OK_O2L_CUBE, M4_RL0, M4_RL1, -32768,
    -16384, 3, M4_RM0, M4_RM1, M4_RS, M4_GL, M4_GM0, M4_GM1, M4_GS, M4_BL, M4_BM, 3, M4_BS0, M4_BS1};

// Work area of the array forms: three rows of 8 and the array pointers of the current 8 elements.
struct alignas(16) ok_ctx
{
    int16_t        w[3][8]; // byte 0
    const int16_t* in[3];   // byte 48
    int16_t*       out[3];  // byte 60
};
static_assert(offsetof(ok_ctx, in) == 48 && offsetof(ok_ctx, out) == 60, "the blocks read these offsets");
} // namespace

// One step of the q15 cube root: x times 8 where below 4096, S doubled where not. q0 = x, q3 = S,
// q4 = 4096, q5 = 4095, q6 = 7, SAR 0.
#define OK_CBRT_STEP                                                                                               \
    "ee.vcmp.lt.s16    q1, q0, q4         \n"                                                                     \
    "ee.vmul.s16       q2, q0, q6         \n"                                                                     \
    "ee.vcmp.gt.s16    q7, q0, q5         \n"                                                                     \
    "ee.andq           q2, q2, q1         \n"                                                                     \
    "ee.andq           q7, q7, q3         \n"                                                                     \
    "ee.vadds.s16      q0, q0, q2         \n"                                                                     \
    "ee.vadds.s16      q3, q3, q7         \n"

// 8 elements a pass. 104 instructions a pass with the loop. Expected about 110 cycles a pass, 14 per
// element: 3 stalls in the model (2 in the core, 1 at the store) and the taken branch.
void tone_fx_cbrt(const q15* x, int n, q15* y)
{
    const q15* px = x;
    q15*       py = y;
    for(int i = 0; i < n; i += 8)
    {
        const int16_t* pk = k_cbrt_stream;
        int            t0, t1;
        __asm__ volatile(
            // SAR 0 for 7x
            "movi              %[t0], 0           \n"
            "ee.vld.128.ip     q0, %[px], 16      \n"
            "wsr.sar           %[t0]              \n"
            "ee.zero.q         q7                 \n"
            "ee.vldbc.16.ip    q4, %[pk], 2       \n" // 4096
            "ee.vmax.s16       q0, q0, q7         \n" // x >= 0
            "ee.vldbc.16.ip    q5, %[pk], 2       \n" // 4095
            "ee.vcmp.gt.s16    q1, q0, q7         \n" // x > 0
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // 7
            "ee.vldbc.16.ip    q3, %[pk], 2       \n" // 1024
            "ee.vcmp.lt.s16    q2, q0, q4         \n" // step 1: m (in q2 here, q1 holds x > 0)
            "ee.andq           q3, q3, q1         \n" // S
            "ee.vmul.s16       q1, q0, q6         \n" // 7x
            "ee.vcmp.gt.s16    q7, q0, q5         \n" // not m
            "ee.andq           q1, q1, q2         \n"
            "ee.andq           q7, q7, q3         \n"
            "ee.vadds.s16      q0, q0, q1         \n"
            "ee.vadds.s16      q3, q3, q7         \n"
            OK_CBRT_STEP OK_CBRT_STEP OK_CBRT_STEP
            "ee.vldbc.16.ip    q4, %[pk], 2       \n" // 16384
            "ee.vldbc.16.ip    q5, %[pk], 2       \n" // 8192
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // 24576
            "ee.vcmp.lt.s16    q1, q0, q4         \n" // mA
            "ee.vcmp.lt.s16    q2, q0, q5         \n" // mB
            "movi              %[t0], 15          \n"
            "ee.andq           q7, q0, q2         \n"
            "wsr.sar           %[t0]              \n"
            "ee.vadds.s16      q0, q0, q7         \n"
            "ee.andq           q7, q0, q1         \n"
            "ee.vadds.s16      q0, q0, q7         \n"
            "ee.vsubs.s16      q0, q0, q6         \n" // d
            OK_CORE(14)
            "ee.vst.128.ip     q0, %[py], 16      \n"
            : [px] "+r"(px), [py] "+r"(py), [pk] "+r"(pk), [t0] "=&r"(t0), [t1] "=&r"(t1)
            :
            : "memory");
    }
}

// 8 pixels a pass: three rows of LMS through the level reads and the core, each cube root to the
// work area, then the second matrix. 685 instructions a pass (203 a row). Expected about 700 cycles
// a pass, 88 per pixel: 7 stalls in the model (2 in each core, 1 at the last store) and 3 taken
// branches.
void tone_fx_lin_to_oklab(const q15* r, const q15* g, const q15* b, int n, q15* L, q15* A, q15* B)
{
    ok_ctx c;
    for(int i = 0; i < n; i += 8)
    {
        c.in[0] = r + i, c.in[1] = g + i, c.in[2] = b + i;
        c.out[0] = L + i, c.out[1] = A + i, c.out[2] = B + i;
        const int16_t* pm = k_l2o_rows;
        const int16_t* pk;
        int16_t*       pw;
        int            t0, t1, cnt;
        __asm__ volatile(
            "movi              %[t0], 15          \n"
            "movi              %[cnt], 3          \n"
            "wsr.sar           %[t0]              \n"
            "mov               %[pw], %[ctx]      \n"
            "1:                                   \n"
            "ee.zero.qacc                         \n"
            "l32i              %[t0], %[ctx], 48  \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n"
            "l32i              %[t1], %[ctx], 52  \n"
            "ee.vld.128.ip     q0, %[t0], 0       \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n"
            "l32i              %[t0], %[ctx], 56  \n"
            "ee.vld.128.ip     q1, %[t1], 0       \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q0, q3            \n"
            "ee.vld.128.ip     q2, %[t0], 0       \n"
            "mov               %[pk], %[kb]       \n"
            "ee.vmulas.s16.qacc q1, q4            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // 0xC000
            "ee.vmulas.s16.qacc q2, q5            \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n" // -16385
            "movi              %[t0], 0           \n"
            OK_LMS_CBRT
            "addi              %[cnt], %[cnt], -1 \n"
            "ee.vst.128.ip     q0, %[pw], 16      \n"
            "bnez              %[cnt], 1b         \n"
            // second matrix: L, then b, then a, each rounded at shift 16
            "mov               %[pw], %[ctx]      \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n" // L: 6896
            "ee.vld.128.ip     q0, %[pw], 16      \n" // wl
            "ee.vldbc.16.ip    q4, %[pm], 2       \n" // 26005
            "ee.vld.128.ip     q1, %[pw], 16      \n" // wm
            "ee.vldbc.16.ip    q5, %[pm], 2       \n" // -133
            "ee.vld.128.ip     q2, %[pw], 0       \n" // ws
            "ee.vldbc.16.ip    q6, %[pm], 2       \n" // 128
            "ee.zero.qacc                         \n"
            "ee.vldbc.16.ip    q7, %[pm], 2       \n" // 256
            "ee.vmulas.s16.qacc q0, q3            \n"
            "ee.vmulas.s16.qacc q1, q4            \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n" // -32768
            "ee.vmulas.s16.qacc q2, q5            \n"
            "ee.vmulas.s16.qacc q6, q7            \n" // rounding half
            "ee.vldbc.16.ip    q5, %[pm], 2       \n" // b: 849
            "ee.vmulas.s16.qacc q4, q4            \n" // 2^30
            "movi              %[t0], 16          \n"
            "ee.srcmb.s16.qacc q3, %[t0], 0       \n" // L
            "ee.vldbc.16.ip    q4, %[pm], 2       \n" // 25650
            "ee.zero.qacc                         \n"
            "l32i              %[t1], %[ctx], 60  \n"
            "ee.vmulas.s16.qacc q0, q5            \n"
            "ee.vst.128.ip     q3, %[t1], 0       \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n" // -26499
            "ee.vmulas.s16.qacc q1, q4            \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n" // a: 32407
            "ee.vmulas.s16.qacc q2, q3            \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n" // 32408
            "ee.vmulas.s16.qacc q6, q7            \n"
            "l32i              %[t1], %[ctx], 68  \n"
            "ee.srcmb.s16.qacc q3, %[t0], 0       \n" // b
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q0, q5            \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n" // -26527
            "ee.vst.128.ip     q3, %[t1], 0       \n"
            "ee.vmulas.s16.qacc q0, q4            \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n" // -26526
            "ee.vmulas.s16.qacc q1, q5            \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n" // 14765
            "ee.vmulas.s16.qacc q1, q5            \n"
            "ee.vmulas.s16.qacc q1, q3            \n"
            "ee.vmulas.s16.qacc q2, q4            \n"
            "ee.vmulas.s16.qacc q6, q7            \n"
            "l32i              %[t1], %[ctx], 64  \n"
            "ee.srcmb.s16.qacc q0, %[t0], 0       \n" // a
            "ee.vst.128.ip     q0, %[t1], 0       \n"
            : [pm] "+r"(pm), [pk] "=&r"(pk), [pw] "=&r"(pw), [t0] "=&r"(t0), [t1] "=&r"(t1), [cnt] "=&r"(cnt)
            : [ctx] "r"(&c), [kb] "r"(k_lms_stream), [one] "r"(1)
            : "memory");
    }
}

// One pixel: LMS in lanes 0 to 2, one pass of the level reads and the core, then the second matrix
// by columns. 230 instructions with the call. Expected about 233 cycles a call, 3 stalls in the
// model. Issue bound: the longest dependent chain (through the accumulator reads and the core) is
// about 80 cycles, so the instruction count is the critical path.
void tone_fx_lin_to_oklab_1(const q15 rgb[8], q15 lab[8])
{
    alignas(16) int16_t w[8];
    const int16_t*      prgb = rgb;
    const int16_t*      pc   = k_l2o1_cols;
    const int16_t*      pk   = k_lms_stream;
    int16_t*            pw   = w;
    q15*                pl   = lab;
    int                 t0, t1;
    __asm__ volatile(
        "movi              %[t0], 15          \n"
        "ee.zero.qacc                         \n"
        "wsr.sar           %[t0]              \n"
        "ee.vldbc.16.ip    q0, %[prgb], 2     \n" // r in every lane
        "ee.vld.128.ip     q3, %[pc], 16      \n" // M1 column r
        "ee.vldbc.16.ip    q1, %[prgb], 2     \n"
        "ee.vld.128.ip     q4, %[pc], 16      \n"
        "ee.vldbc.16.ip    q2, %[prgb], 2     \n"
        "ee.vld.128.ip     q5, %[pc], 16      \n"
        "ee.vmulas.s16.qacc q0, q3            \n"
        "ee.vldbc.16.ip    q6, %[pk], 2       \n" // 0xC000
        "ee.vmulas.s16.qacc q1, q4            \n"
        "ee.vldbc.16.ip    q7, %[pk], 2       \n" // -16385
        "ee.vmulas.s16.qacc q2, q5            \n"
        "movi              %[t0], 0           \n"
        OK_LMS_CBRT
        "ee.vld.128.ip     q3, %[pc], 16      \n" // L, a, b column of l
        "ee.vld.128.ip     q4, %[pc], 16      \n" // second part of a's l
        "ee.vst.128.ip     q0, %[pw], 0       \n"
        "ee.vld.128.ip     q5, %[pc], 16      \n" // column of m
        "ee.vldbc.16.ip    q0, %[pw], 2       \n" // wl in every lane
        "ee.vld.128.ip     q6, %[pc], 16      \n" // second part of a's m
        "ee.vldbc.16.ip    q1, %[pw], 2       \n" // wm
        "ee.zero.qacc                         \n"
        "ee.vldbc.16.ip    q2, %[pw], 2       \n" // ws
        "ee.vmulas.s16.qacc q0, q3            \n"
        "ee.vld.128.ip     q7, %[pc], 16      \n" // third part of a's m
        "ee.vmulas.s16.qacc q0, q4            \n"
        "ee.vld.128.ip     q3, %[pc], 16      \n" // column of s
        "ee.vmulas.s16.qacc q1, q5            \n"
        "ee.vld.128.ip     q4, %[pc], 16      \n" // -32768 in lane 0
        "ee.vmulas.s16.qacc q1, q6            \n"
        "ee.vldbc.16.ip    q5, %[pc], 2       \n" // 128
        "ee.vmulas.s16.qacc q1, q7            \n"
        "ee.vldbc.16.ip    q6, %[pc], 2       \n" // 256
        "ee.vmulas.s16.qacc q2, q3            \n"
        "ee.vldbc.16.ip    q7, %[pc], 2       \n" // -32768
        "ee.vmulas.s16.qacc q5, q6            \n" // rounding half
        "ee.vmulas.s16.qacc q4, q7            \n" // 2^30 in lane 0
        "movi              %[t0], 16          \n"
        "ee.srcmb.s16.qacc q0, %[t0], 0       \n"
        "ee.vst.128.ip     q0, %[pl], 0       \n"
        : [prgb] "+r"(prgb), [pc] "+r"(pc), [pk] "+r"(pk), [pw] "+r"(pw), [pl] "+r"(pl), [t0] "=&r"(t0),
          [t1] "=&r"(t1)
        : [one] "r"(1)
        : "memory");
}

// One channel of Oklab to linear. In: q0 = L, q1 = A, q2 = B, q3 = 16384, q6 = -32640, q7 = 16448
// (loaded by the previous stage), %[pk] at the channel's stream. ROWB adds the B terms of the row in
// the first pass, ROWB2 those of the second. Out: the offset q16 cube to the work area, and q6, q7,
// q3 loaded with the next 3 stream values for the next stage. SAR is 0 for pl and 16 for ph and h.
#define OK_O2L_CHANNEL(ROWB, ROWB2)                                                                                \
    "ee.zero.qacc                         \n"                                                                     \
    "ee.vmulas.s16.qacc q0, q3            \n"                                                                     \
    "ee.vldbc.16.ip    q4, %[pk], 2       \n" /* row A */                                                         \
    "ee.vmulas.s16.qacc q0, q3            \n"                                                                     \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* row B */                                                         \
    "ee.vmulas.s16.qacc q6, q7            \n" /* -2^29 + 2^13 */                                                  \
    "ee.vmulas.s16.qacc q1, q4            \n"                                                                     \
    ROWB                                                                                                           \
    "movi              %[t0], 14          \n"                                                                     \
    "ee.srcmb.s16.qacc q6, %[t0], 0       \n" /* w */                                                             \
    "ee.zero.qacc                         \n"                                                                     \
    "ee.vmulas.s16.qacc q0, q3            \n"                                                                     \
    "ee.vmulas.s16.qacc q0, q3            \n"                                                                     \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* -32768 */                                                        \
    "ee.vmulas.s16.qacc q1, q4            \n"                                                                     \
    "ee.vldbc.16.ip    q4, %[pk], 2       \n" /* -16384 */                                                        \
    ROWB2                                                                                                          \
    "ee.vmulas.s16.qacc q7, q3            \n" /* -2^29 */                                                         \
    "ee.vmulas.s16.qacc q6, q4            \n" /* -w * 2^14 */                                                     \
    "movi              %[t0], 1           \n"                                                                     \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* 0x8000 */                                                        \
    "ee.srcmb.s16.qacc q7, %[t0], 0       \n" /* e2 */                                                            \
    "movi              %[t0], 0           \n"                                                                     \
    "ee.xorq           q6, q6, q5         \n" /* u */                                                             \
    "wsr.sar           %[t0]              \n"                                                                     \
    "ee.vadds.s16      q4, q7, q7         \n"                                                                     \
    "ee.vmul.s16       q3, q6, q6         \n" /* pl */                                                            \
    "ee.vadds.s16      q4, q4, q7         \n" /* e3 */                                                            \
    "movi              %[t0], 16          \n"                                                                     \
    "ee.zero.qacc                         \n"                                                                     \
    "wsr.sar           %[t0]              \n"                                                                     \
    "ee.vmulas.u16.qacc q3, q6            \n" /* pl * u */                                                        \
    "ee.vmul.u16       q7, q6, q6         \n" /* ph */                                                            \
    "movi              %[t0], 4           \n"                                                                     \
    "ee.srcmb.s16.qacc q3, %[t0], 0       \n" /* >> 4, value unused */                                            \
    "ee.vmul.u16       q5, q7, q5         \n" /* h = ph / 2 */                                                    \
    "ee.vldbc.16.ip    q3, %[pk], 2       \n" /* -32768 */                                                        \
    "ee.vmulas.s16.qacc q5, q4            \n" /* h * e3 */                                                        \
    "ee.vldbc.16.ip    q4, %[pk], 2       \n" /* -16384 */                                                        \
    "movi              %[t0], 12          \n"                                                                     \
    "ee.vmulas.s16.qacc q3, q4            \n" /* 2^29 */                                                          \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* 32767 */                                                         \
    "ee.srcmb.s16.qacc q4, %[t0], 0       \n" /* >> 12, value unused */                                           \
    "movi              %[t0], 16          \n"                                                                     \
    "ee.vmulas.u16.qacc q7, q6            \n" /* ph * u */                                                        \
    "ee.vmulas.s16.qacc q3, q5            \n"                                                                     \
    "ee.vmulas.s16.qacc q3, q5            \n" /* -2^31 + 2^16 */                                                  \
    "ee.vldbc.16.ip    q6, %[pk], 2       \n" /* next stage */                                                    \
    "ee.srcmb.s16.qacc q4, %[t0], 0       \n" /* cube */                                                          \
    "ee.vldbc.16.ip    q7, %[pk], 2       \n" /* next stage */                                                    \
    "ee.vldbc.16.ip    q3, %[pk], 2       \n" /* next stage */                                                    \
    "ee.vst.128.ip     q4, %[pw], 16      \n"
#define OK_ROWB "ee.vmulas.s16.qacc q2, q5 \n"
#define OK_ROWB_S                                                                                                  \
    "ee.vmulas.s16.qacc q2, q5            \n"                                                                     \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* second part of row B */                                          \
    "movi              %[t0], 14          \n"                                                                     \
    "ee.vmulas.s16.qacc q2, q5            \n"
#define OK_ROWB2_S                                                                                                 \
    "ee.vmulas.s16.qacc q2, q5            \n"                                                                     \
    "ee.vldbc.16.ip    q5, %[pk], 2       \n" /* first part again */                                              \
    "movi              %[t0], 1           \n"                                                                     \
    "ee.vmulas.s16.qacc q2, q5            \n"

// 8 elements a pass. Per channel the row twice (value and residue), the cube, to the work area,
// then the last matrix. 252 instructions a pass with the loop. Expected about 256 cycles a pass, 32
// per element: 1 stall in the model (the last read and its clamp) and the taken branch.
void tone_fx_oklab_to_lin(const q15* L, const q15* A, const q15* B, int n, q15* r, q15* g, q15* b)
{
    ok_ctx c;
    for(int i = 0; i < n; i += 8)
    {
        c.in[0] = L + i, c.in[1] = A + i, c.in[2] = B + i;
        c.out[0] = r + i, c.out[1] = g + i, c.out[2] = b + i;
        const int16_t* pk = k_o2l_stream;
        int16_t*       pw;
        int            t0, t1;
        __asm__ volatile(
            "l32i              %[t1], %[ctx], 48  \n"
            "ee.vldbc.16.ip    q3, %[pk], 2       \n" // 16384
            "l32i              %[t0], %[ctx], 52  \n"
            "ee.vld.128.ip     q0, %[t1], 0       \n" // L
            "l32i              %[t1], %[ctx], 56  \n"
            "ee.vld.128.ip     q1, %[t0], 0       \n" // A
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // -32640
            "ee.vld.128.ip     q2, %[t1], 0       \n" // B
            "ee.vldbc.16.ip    q7, %[pk], 2       \n" // 16448
            "mov               %[pw], %[ctx]      \n"
            OK_O2L_CHANNEL(OK_ROWB, OK_ROWB)
            OK_O2L_CHANNEL(OK_ROWB, OK_ROWB)
            OK_O2L_CHANNEL(OK_ROWB_S, OK_ROWB2_S)
            // Last matrix, each row with 2^30 - 49152 and read out at shift 16, held at 0 and up.
            // The s channel left q6 = RL0, q7 = RL1, q3 = -32768.
            "mov               %[pw], %[ctx]      \n"
            "ee.vld.128.ip     q0, %[pw], 16      \n" // cube of l
            "ee.vldbc.16.ip    q4, %[pk], 2       \n" // -16384
            "ee.vld.128.ip     q1, %[pw], 16      \n" // m
            "ee.vldbc.16.ip    q5, %[pk], 2       \n" // 3
            "ee.vld.128.ip     q2, %[pw], 0       \n" // s
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q3, q3            \n"
            "ee.vmulas.s16.qacc q0, q6            \n" // r
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q4, q5            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // RM0
            "ee.vmulas.s16.qacc q0, q7            \n"
            "ee.vmulas.s16.qacc q0, q7            \n"
            "ee.vmulas.s16.qacc q0, q7            \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n" // RM1
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // RS
            "ee.vmulas.s16.qacc q1, q7            \n"
            "movi              %[t0], 16          \n"
            "ee.vmulas.s16.qacc q2, q6            \n"
            "l32i              %[t1], %[ctx], 60  \n"
            "ee.srcmb.s16.qacc q6, %[t0], 0       \n"
            "ee.zero.q         q7                 \n"
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q3, q3            \n"
            "ee.vmax.s16       q6, q6, q7         \n"
            "ee.vmulas.s16.qacc q4, q5            \n"
            "ee.vst.128.ip     q6, %[t1], 0       \n" // r
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // g: GL
            "ee.vldbc.16.ip    q7, %[pk], 2       \n" // GM0
            "l32i              %[t1], %[ctx], 64  \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q1, q7            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // GM1
            "ee.vmulas.s16.qacc q1, q7            \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n" // GS
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // b: BL
            "ee.vmulas.s16.qacc q2, q7            \n"
            "ee.zero.q         q7                 \n"
            "ee.srcmb.s16.qacc q5, %[t0], 0       \n" // g, over the 3 of the offset
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q3, q3            \n"
            "ee.vmulas.s16.qacc q0, q6            \n" // BL
            "ee.vmax.s16       q5, q5, q7         \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // BM
            "ee.vst.128.ip     q5, %[t1], 0       \n" // g
            "ee.vldbc.16.ip    q5, %[pk], 2       \n" // 3 again
            "ee.vldbc.16.ip    q3, %[pk], 2       \n" // BS0
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n" // BS1
            "ee.vmulas.s16.qacc q4, q5            \n"
            "ee.vmulas.s16.qacc q2, q3            \n"
            "l32i              %[t1], %[ctx], 68  \n"
            "ee.vmulas.s16.qacc q2, q6            \n"
            "ee.zero.q         q7                 \n"
            "ee.srcmb.s16.qacc q6, %[t0], 0       \n"
            "ee.vmax.s16       q6, q6, q7         \n"
            "ee.vst.128.ip     q6, %[t1], 0       \n" // b
            : [pk] "+r"(pk), [pw] "=&r"(pw), [t0] "=&r"(t0), [t1] "=&r"(t1)
            : [ctx] "r"(&c)
            : "memory");
    }
}
#undef OK_O2L_CHANNEL
#undef OK_ROWB
#undef OK_ROWB_S
#undef OK_ROWB2_S
#endif // TONE_FX_PIE
