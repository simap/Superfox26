#include "tone_fx_srgb.h"

#if TONE_FX_PIE

#include "tone_fx_srgb_internal.h"

namespace
{
using namespace tone_fx_srgb_internal;

alignas(16) const uint8_t k_x80[16] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                       0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};

alignas(16) const int16_t k_curve[8] = {K, CLAMP, C5, C4, C3, C2, C1, C0};

void deinterleave_pie(const uint8_t* src, int blocks, q15* r, q15* g, q15* b)
{
    const uint8_t* k = k_x80;
    __asm__ volatile(
        "ee.ld.128.usar.ip q0, %[src], 16 \n"
        "ee.vld.128.ip     q7, %[k], 0    \n"
        "1:                               \n"
        "ee.vld.128.ip     q1, %[src], 16 \n"
        "ee.vld.128.ip     q2, %[src], 16 \n"
        "ee.src.q          q3, q0, q1     \n"
        "ee.vld.128.ip     q0, %[src], 16 \n"
        "ee.src.q          q4, q1, q2     \n"
        "ee.xorq           q3, q3, q7     \n"
        "ee.src.q          q5, q2, q0     \n"
        "ee.xorq           q4, q4, q7     \n"
        "ee.xorq           q5, q5, q7     \n"
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        "ee.orq            q5, q1, q1     \n"
        "ee.srci.2q        q4, q5, 7      \n"
        "ee.vzip.8         q3, q5         \n"
        "ee.vzip.8         q1, q4         \n"
        "ee.orq            q4, q5, q5     \n"
        "ee.srci.2q        q1, q4, 7      \n"
        "ee.vzip.8         q3, q4         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        "ee.zero.q         q2             \n"
        "ee.zero.q         q5             \n"
        "ee.zero.q         q6             \n"
        "ee.vzip.8         q2, q3         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.vzip.8         q6, q4         \n"
        "ee.vst.128.ip     q2, %[r], 16   \n"
        "ee.vst.128.ip     q3, %[r], 16   \n"
        "ee.vst.128.ip     q5, %[g], 16   \n"
        "ee.vst.128.ip     q1, %[g], 16   \n"
        "ee.vst.128.ip     q6, %[b], 16   \n"
        "ee.vst.128.ip     q4, %[b], 16   \n"
        "addi              %[n], %[n], -1 \n"
        "bnez              %[n], 1b       \n"
        : [src] "+r"(src), [n] "+r"(blocks), [r] "+r"(r), [g] "+r"(g), [b] "+r"(b), [k] "+r"(k)
        :
        : "memory");
}

void deinterleave8_pie(const uint8_t* src, q15* r, q15* g, q15* b)
{
    const uint8_t* k    = k_x80;
    const uint8_t* last = src + 23;
    __asm__ volatile(
        "ee.ld.128.usar.ip q0, %[src], 16 \n"
        "ee.vld.128.ip     q1, %[src], 0  \n"
        "ee.vld.128.ip     q2, %[last], 0 \n"
        "ee.vld.128.ip     q7, %[k], 0    \n"
        "ee.src.q          q3, q0, q1     \n"
        "ee.src.q          q4, q1, q2     \n"
        "ee.zero.q         q5             \n"
        "ee.xorq           q3, q3, q7     \n"
        "ee.xorq           q4, q4, q7     \n"
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        "ee.orq            q5, q1, q1     \n"
        "ee.srci.2q        q4, q5, 7      \n"
        "ee.vzip.8         q3, q5         \n"
        "ee.vzip.8         q1, q4         \n"
        "ee.orq            q4, q5, q5     \n"
        "ee.srci.2q        q1, q4, 7      \n"
        "ee.vzip.8         q3, q4         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        "ee.zero.q         q2             \n"
        "ee.zero.q         q5             \n"
        "ee.zero.q         q6             \n"
        "ee.vzip.8         q2, q3         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.vzip.8         q6, q4         \n"
        "ee.vst.128.ip     q2, %[r], 0    \n"
        "ee.vst.128.ip     q5, %[g], 0    \n"
        "ee.vst.128.ip     q6, %[b], 0    \n"
        : [src] "+r"(src), [last] "+r"(last), [r] "+r"(r), [g] "+r"(g), [b] "+r"(b), [k] "+r"(k)
        :
        : "memory");
}

void curve_pie(q15* a, int pairs)
{
    const int16_t* k   = k_curve;
    q15*           out = a;
    __asm__ volatile(
        "ssai              15             \n"
        "1:                               \n"
        "ee.vld.128.ip     q0, %[a], 16   \n"
        "ee.vld.128.ip     q1, %[a], 16   \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ee.vmul.s16       q2, q0, q6     \n"
        "ee.vmul.s16       q3, q1, q6     \n"
        "ee.vmax.s16       q0, q0, q7     \n"
        "ee.vmax.s16       q1, q1, q7     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ee.vmul.s16       q4, q0, q6     \n"
        "ee.vmul.s16       q5, q1, q6     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ee.vadds.s16      q4, q4, q7     \n"
        "ee.vadds.s16      q5, q5, q7     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ssai              19             \n"
        "ee.vadds.s16      q4, q4, q6     \n"
        "ee.vadds.s16      q5, q5, q6     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ssai              15             \n"
        "ee.vadds.s16      q4, q4, q7     \n"
        "ee.vadds.s16      q5, q5, q7     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "addi              %[k], %[k], -16\n"
        "ee.vadds.s16      q4, q4, q6     \n"
        "ee.vadds.s16      q5, q5, q6     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "addi              %[n], %[n], -1 \n"
        "ee.vadds.s16      q4, q4, q2     \n"
        "ee.vadds.s16      q5, q5, q3     \n"
        "ee.vadds.s16      q4, q4, q7     \n"
        "ee.vadds.s16      q5, q5, q7     \n"
        "ee.vst.128.ip     q4, %[o], 16   \n"
        "ee.vst.128.ip     q5, %[o], 16   \n"
        "bnez              %[n], 1b       \n"
        : [a] "+r"(a), [o] "+r"(out), [k] "+r"(k), [n] "+r"(pairs)
        :
        : "memory");
}

void curve8_pie(q15* a)
{
    const int16_t* k = k_curve;
    __asm__ volatile(
        "ssai              15             \n"
        "ee.vld.128.ip     q0, %[a], 0    \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ee.vmul.s16       q2, q0, q6     \n"
        "ee.vmax.s16       q0, q0, q7     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ee.vmul.s16       q4, q0, q6     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ee.vadds.s16      q4, q4, q7     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ssai              19             \n"
        "ee.vadds.s16      q4, q4, q6     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n"
        "ssai              15             \n"
        "ee.vadds.s16      q4, q4, q7     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n"
        "ee.vadds.s16      q4, q4, q6     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vadds.s16      q4, q4, q2     \n"
        "ee.vadds.s16      q4, q4, q7     \n"
        "ee.vst.128.ip     q4, %[a], 0    \n"
        : [a] "+r"(a), [k] "+r"(k)
        :
        : "memory");
}

void curve_all_pie(q15* a, int n)
{
    if(n >= 16) curve_pie(a, n / 16);
    if(n % 16) curve8_pie(a + n - 8);
}
}

void tone_fx_srgb_to_lin(const uint8_t* rgb, int n, q15* r, q15* g, q15* b)
{
    const int blocks = n / 16;
    if(blocks > 0) deinterleave_pie(rgb, blocks, r, g, b);
    if(n % 16)
    {
        const int done = 16 * blocks;
        deinterleave8_pie(rgb + 3 * done, r + done, g + done, b + done);
    }
    curve_all_pie(r, n);
    curve_all_pie(g, n);
    curve_all_pie(b, n);
}

#endif
