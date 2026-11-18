// Each kernel reads its constant table through one pointer that advances in step with the
// instruction order, so the values in a table must stay in the same order as the loads of the
// kernel that consumes it.
// The kernels reach ok_ctx fields by fixed byte offset: in at 48, 52 and 56, out at 60, 64 and 68,
// with w as 16-byte aligned scratch at offset 0.
// The static_assert guards these offsets.
#include "tone_fx_oklab.h"

#if TONE_FX_PIE

#include <cstddef>
#include <cstdint>

#include "tone_fx_oklab_internal.h"

namespace
{
using namespace tone_fx_oklab_internal;

#define OK_STR2(x) #x
#define OK_STR(x) OK_STR2(x)

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

#define OK_SRC "ee.srcmb.s16.qacc q0, %[one], 0 \n"
#define OK_DBL "ee.vadds.s16      q0, q0, q0     \n"
#define OK_LV(NEXT)                                                                                                \
    NEXT "ee.vmin.s16       q2, q2, q1     \n"                                                                     \
         "ee.vcmp.gt.s16    q3, q1, q7     \n"                                                                     \
         "ee.xorq           q1, q0, q6     \n"                                                                     \
         "ee.vadds.s16      q4, q4, q3     \n"
#define OK_LV_BOTTOM(NEXT)                                                                                         \
    NEXT "ee.vmin.s16       q2, q2, q1     \n"                                                                     \
         "ee.vcmp.gt.s16    q3, q1, q7     \n"                                                                     \
         "ee.vldbc.16.ip    q1, %[pk], 2   \n"                                                                     \
         "ee.vsubs.s16      q4, q4, q3     \n"                                                                     \
         "ee.andq           q1, q1, q3     \n"                                                                     \
         "ee.vsubs.s16      q4, q4, q3     \n"                                                                     \
         "ee.vsubs.s16      q5, q5, q1     \n"                                                                     \
         "ee.xorq           q1, q0, q6     \n"
#define OK_LV_ZERO(NEXT)                                                                                           \
    NEXT "ee.vmin.s16       q2, q2, q1     \n"                                                                     \
         "ee.vcmp.gt.s16    q3, q1, q7     \n"                                                                     \
         "ee.vldbc.16.ip    q1, %[pk], 2   \n"                                                                     \
         "ee.vadds.s16      q4, q4, q3     \n"                                                                     \
         "ee.andq           q1, q1, q3     \n"                                                                     \
         "ee.vsubs.s16      q5, q5, q1     \n"                                                                     \
         "ee.xorq           q1, q0, q6     \n"

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

#define OK_LMS_STREAM                                                                                              \
    static_cast<int16_t>(0xC000), -16385, 16384, 256, 256, 512, 1024, 2048, 4096, 8192, 24576,                    \
        OK_CORE_STREAM(PATH_FLOOR)

alignas(16) const int16_t k_cbrt_stream[] = {4096, 4095, 7, 1024, 16384, 8192, 24576, OK_CORE_STREAM(PATH_EXACT)};
alignas(16) const int16_t k_lms_stream[]  = {OK_LMS_STREAM};
alignas(16) const int16_t k_l2o_rows[] = {M1[0][0], M1[0][1], M1[0][2], M1[1][0], M1[1][1], M1[1][2], M1[2][0], M1[2][1],
                                         M1[2][2], M2_L[0],  M2_L[1],  M2_L[2],  128,      256,      -32768,   M2_B[0],
                                         M2_B[1],  M2_B[2],  M2_A_L[0], M2_A_L[1], M2_A_M[0], M2_A_M[2], M2_A_S};
alignas(16) const int16_t k_l2o1_cols[] = {
    M1[0][0], M1[1][0], M1[2][0], 0, 0, 0, 0, 0, M1[0][1], M1[1][1], M1[2][1], 0, 0, 0, 0, 0,
    M1[0][2], M1[1][2], M1[2][2], 0, 0, 0, 0, 0, M2_L[0], M2_A_L[0], M2_B[0], 0, 0, 0, 0, 0,
    0, M2_A_L[1], 0, 0, 0, 0, 0, 0, M2_L[1], M2_A_M[0], M2_B[1], 0, 0, 0, 0, 0,
    0, M2_A_M[1], 0, 0, 0, 0, 0, 0, 0, M2_A_M[2], 0, 0, 0, 0, 0, 0,
    M2_L[2], M2_A_S, M2_B[2], 0, 0, 0, 0, 0, -32768, 0, 0, 0, 0, 0, 0, 0,
    128, 256, -32768, 0, 0, 0, 0, 0};

#define OK_O2L_CUBE static_cast<int16_t>(0x8000), -32768, -16384, 32767
alignas(16) const int16_t k_o2l_stream[] = {
    16384, -32640, 16448,
    M3_LA, M3_LB, -32768, -16384, OK_O2L_CUBE, -32640, 16448, 16384,
    M3_MA, M3_MB, -32768, -16384, OK_O2L_CUBE, -32640, 16448, 16384,
    M3_SA, M3_SB0, M3_SB1, -32768, -16384, M3_SB0, OK_O2L_CUBE, M4_RL0, M4_RL1, -32768,
    -16384, 3, M4_RM0, M4_RM1, M4_RS, M4_GL, M4_GM0, M4_GM1, M4_GS, M4_BL, M4_BM, 3, M4_BS0, M4_BS1};

struct alignas(16) ok_ctx
{
    int16_t        w[3][8];
    const int16_t* in[3];
    int16_t*       out[3];
};
static_assert(offsetof(ok_ctx, in) == 48 && offsetof(ok_ctx, out) == 60, "the blocks read these offsets");
}

#define OK_CBRT_STEP                                                                                               \
    "ee.vcmp.lt.s16    q1, q0, q4         \n"                                                                     \
    "ee.vmul.s16       q2, q0, q6         \n"                                                                     \
    "ee.vcmp.gt.s16    q7, q0, q5         \n"                                                                     \
    "ee.andq           q2, q2, q1         \n"                                                                     \
    "ee.andq           q7, q7, q3         \n"                                                                     \
    "ee.vadds.s16      q0, q0, q2         \n"                                                                     \
    "ee.vadds.s16      q3, q3, q7         \n"

void tone_fx_cbrt(const q15* x, int n, q15* y)
{
    const q15* px = x;
    q15*       py = y;
    for(int i = 0; i < n; i += 8)
    {
        const int16_t* pk = k_cbrt_stream;
        int            t0, t1;
        __asm__ volatile(
            "movi              %[t0], 0           \n"
            "ee.vld.128.ip     q0, %[px], 16      \n"
            "wsr.sar           %[t0]              \n"
            "ee.zero.q         q7                 \n"
            "ee.vldbc.16.ip    q4, %[pk], 2       \n"
            "ee.vmax.s16       q0, q0, q7         \n"
            "ee.vldbc.16.ip    q5, %[pk], 2       \n"
            "ee.vcmp.gt.s16    q1, q0, q7         \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vldbc.16.ip    q3, %[pk], 2       \n"
            "ee.vcmp.lt.s16    q2, q0, q4         \n"
            "ee.andq           q3, q3, q1         \n"
            "ee.vmul.s16       q1, q0, q6         \n"
            "ee.vcmp.gt.s16    q7, q0, q5         \n"
            "ee.andq           q1, q1, q2         \n"
            "ee.andq           q7, q7, q3         \n"
            "ee.vadds.s16      q0, q0, q1         \n"
            "ee.vadds.s16      q3, q3, q7         \n"
            OK_CBRT_STEP OK_CBRT_STEP OK_CBRT_STEP
            "ee.vldbc.16.ip    q4, %[pk], 2       \n"
            "ee.vldbc.16.ip    q5, %[pk], 2       \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vcmp.lt.s16    q1, q0, q4         \n"
            "ee.vcmp.lt.s16    q2, q0, q5         \n"
            "movi              %[t0], 15          \n"
            "ee.andq           q7, q0, q2         \n"
            "wsr.sar           %[t0]              \n"
            "ee.vadds.s16      q0, q0, q7         \n"
            "ee.andq           q7, q0, q1         \n"
            "ee.vadds.s16      q0, q0, q7         \n"
            "ee.vsubs.s16      q0, q0, q6         \n"
            OK_CORE(14)
            "ee.vst.128.ip     q0, %[py], 16      \n"
            : [px] "+r"(px), [py] "+r"(py), [pk] "+r"(pk), [t0] "=&r"(t0), [t1] "=&r"(t1)
            :
            : "memory");
    }
}

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
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q2, q5            \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n"
            "movi              %[t0], 0           \n"
            OK_LMS_CBRT
            "addi              %[cnt], %[cnt], -1 \n"
            "ee.vst.128.ip     q0, %[pw], 16      \n"
            "bnez              %[cnt], 1b         \n"
            "mov               %[pw], %[ctx]      \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n"
            "ee.vld.128.ip     q0, %[pw], 16      \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n"
            "ee.vld.128.ip     q1, %[pw], 16      \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n"
            "ee.vld.128.ip     q2, %[pw], 0       \n"
            "ee.vldbc.16.ip    q6, %[pm], 2       \n"
            "ee.zero.qacc                         \n"
            "ee.vldbc.16.ip    q7, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q0, q3            \n"
            "ee.vmulas.s16.qacc q1, q4            \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q2, q5            \n"
            "ee.vmulas.s16.qacc q6, q7            \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q4, q4            \n"
            "movi              %[t0], 16          \n"
            "ee.srcmb.s16.qacc q3, %[t0], 0       \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n"
            "ee.zero.qacc                         \n"
            "l32i              %[t1], %[ctx], 60  \n"
            "ee.vmulas.s16.qacc q0, q5            \n"
            "ee.vst.128.ip     q3, %[t1], 0       \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q1, q4            \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q2, q3            \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q6, q7            \n"
            "l32i              %[t1], %[ctx], 68  \n"
            "ee.srcmb.s16.qacc q3, %[t0], 0       \n"
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q0, q5            \n"
            "ee.vldbc.16.ip    q5, %[pm], 2       \n"
            "ee.vst.128.ip     q3, %[t1], 0       \n"
            "ee.vmulas.s16.qacc q0, q4            \n"
            "ee.vldbc.16.ip    q3, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q1, q5            \n"
            "ee.vldbc.16.ip    q4, %[pm], 2       \n"
            "ee.vmulas.s16.qacc q1, q5            \n"
            "ee.vmulas.s16.qacc q1, q3            \n"
            "ee.vmulas.s16.qacc q2, q4            \n"
            "ee.vmulas.s16.qacc q6, q7            \n"
            "l32i              %[t1], %[ctx], 64  \n"
            "ee.srcmb.s16.qacc q0, %[t0], 0       \n"
            "ee.vst.128.ip     q0, %[t1], 0       \n"
            : [pm] "+r"(pm), [pk] "=&r"(pk), [pw] "=&r"(pw), [t0] "=&r"(t0), [t1] "=&r"(t1), [cnt] "=&r"(cnt)
            : [ctx] "r"(&c), [kb] "r"(k_lms_stream), [one] "r"(1)
            : "memory");
    }
}

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
        "ee.vldbc.16.ip    q0, %[prgb], 2     \n"
        "ee.vld.128.ip     q3, %[pc], 16      \n"
        "ee.vldbc.16.ip    q1, %[prgb], 2     \n"
        "ee.vld.128.ip     q4, %[pc], 16      \n"
        "ee.vldbc.16.ip    q2, %[prgb], 2     \n"
        "ee.vld.128.ip     q5, %[pc], 16      \n"
        "ee.vmulas.s16.qacc q0, q3            \n"
        "ee.vldbc.16.ip    q6, %[pk], 2       \n"
        "ee.vmulas.s16.qacc q1, q4            \n"
        "ee.vldbc.16.ip    q7, %[pk], 2       \n"
        "ee.vmulas.s16.qacc q2, q5            \n"
        "movi              %[t0], 0           \n"
        OK_LMS_CBRT
        "ee.vld.128.ip     q3, %[pc], 16      \n"
        "ee.vld.128.ip     q4, %[pc], 16      \n"
        "ee.vst.128.ip     q0, %[pw], 0       \n"
        "ee.vld.128.ip     q5, %[pc], 16      \n"
        "ee.vldbc.16.ip    q0, %[pw], 2       \n"
        "ee.vld.128.ip     q6, %[pc], 16      \n"
        "ee.vldbc.16.ip    q1, %[pw], 2       \n"
        "ee.zero.qacc                         \n"
        "ee.vldbc.16.ip    q2, %[pw], 2       \n"
        "ee.vmulas.s16.qacc q0, q3            \n"
        "ee.vld.128.ip     q7, %[pc], 16      \n"
        "ee.vmulas.s16.qacc q0, q4            \n"
        "ee.vld.128.ip     q3, %[pc], 16      \n"
        "ee.vmulas.s16.qacc q1, q5            \n"
        "ee.vld.128.ip     q4, %[pc], 16      \n"
        "ee.vmulas.s16.qacc q1, q6            \n"
        "ee.vldbc.16.ip    q5, %[pc], 2       \n"
        "ee.vmulas.s16.qacc q1, q7            \n"
        "ee.vldbc.16.ip    q6, %[pc], 2       \n"
        "ee.vmulas.s16.qacc q2, q3            \n"
        "ee.vldbc.16.ip    q7, %[pc], 2       \n"
        "ee.vmulas.s16.qacc q5, q6            \n"
        "ee.vmulas.s16.qacc q4, q7            \n"
        "movi              %[t0], 16          \n"
        "ee.srcmb.s16.qacc q0, %[t0], 0       \n"
        "ee.vst.128.ip     q0, %[pl], 0       \n"
        : [prgb] "+r"(prgb), [pc] "+r"(pc), [pk] "+r"(pk), [pw] "+r"(pw), [pl] "+r"(pl), [t0] "=&r"(t0),
          [t1] "=&r"(t1)
        : [one] "r"(1)
        : "memory");
}

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
            "ee.vldbc.16.ip    q3, %[pk], 2       \n"
            "l32i              %[t0], %[ctx], 52  \n"
            "ee.vld.128.ip     q0, %[t1], 0       \n"
            "l32i              %[t1], %[ctx], 56  \n"
            "ee.vld.128.ip     q1, %[t0], 0       \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vld.128.ip     q2, %[t1], 0       \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n"
            "mov               %[pw], %[ctx]      \n"
            OK_O2L_CHANNEL(OK_ROWB, OK_ROWB)
            OK_O2L_CHANNEL(OK_ROWB, OK_ROWB)
            OK_O2L_CHANNEL(OK_ROWB_S, OK_ROWB2_S)
            "mov               %[pw], %[ctx]      \n"
            "ee.vld.128.ip     q0, %[pw], 16      \n"
            "ee.vldbc.16.ip    q4, %[pk], 2       \n"
            "ee.vld.128.ip     q1, %[pw], 16      \n"
            "ee.vldbc.16.ip    q5, %[pk], 2       \n"
            "ee.vld.128.ip     q2, %[pw], 0       \n"
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q3, q3            \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q4, q5            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q0, q7            \n"
            "ee.vmulas.s16.qacc q0, q7            \n"
            "ee.vmulas.s16.qacc q0, q7            \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
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
            "ee.vst.128.ip     q6, %[t1], 0       \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n"
            "l32i              %[t1], %[ctx], 64  \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmulas.s16.qacc q1, q7            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q1, q7            \n"
            "ee.vldbc.16.ip    q7, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q2, q7            \n"
            "ee.zero.q         q7                 \n"
            "ee.srcmb.s16.qacc q5, %[t0], 0       \n"
            "ee.zero.qacc                         \n"
            "ee.vmulas.s16.qacc q3, q3            \n"
            "ee.vmulas.s16.qacc q0, q6            \n"
            "ee.vmax.s16       q5, q5, q7         \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vst.128.ip     q5, %[t1], 0       \n"
            "ee.vldbc.16.ip    q5, %[pk], 2       \n"
            "ee.vldbc.16.ip    q3, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q1, q6            \n"
            "ee.vldbc.16.ip    q6, %[pk], 2       \n"
            "ee.vmulas.s16.qacc q4, q5            \n"
            "ee.vmulas.s16.qacc q2, q3            \n"
            "l32i              %[t1], %[ctx], 68  \n"
            "ee.vmulas.s16.qacc q2, q6            \n"
            "ee.zero.q         q7                 \n"
            "ee.srcmb.s16.qacc q6, %[t0], 0       \n"
            "ee.vmax.s16       q6, q6, q7         \n"
            "ee.vst.128.ip     q6, %[t1], 0       \n"
            : [pk] "+r"(pk), [pw] "=&r"(pw), [t0] "=&r"(t0), [t1] "=&r"(t1)
            : [ctx] "r"(&c)
            : "memory");
    }
}
#undef OK_O2L_CHANNEL
#undef OK_ROWB
#undef OK_ROWB_S
#undef OK_ROWB2_S
#endif
