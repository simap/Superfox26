// Definitions the squeeze tone effect shares between its coefficient table and the code that
// applies it.
// The K enum names each coefficient by its position in the parameter array, so the scalar functions
// here and the vector kernel in the source file read the same values in one order.
// The order of the K enum is the layout of the parameter array that tone_fx_squeeze_params returns.
// The table in tone_tables and the coefficient loads in the kernel must both follow it.
// With TONEMAP_GAMMA other than 1 the array holds the curve stream after the K_H1B entry. The
// kernel then reads the curve stream, then the entries from K_PM0 to K_H1B.
#pragma once

#include <cstdint>

#include "tonemap.h"

#define SQZ_DIV_BITS 9

namespace tone_fx_squeeze_internal
{
const int DIV_BITS = SQZ_DIV_BITS;

// At TONEMAP_GAMMA 1 the kernel has no tone curve and the array ends at K_H1B.
constexpr bool CURVE = TONEMAP_GAMMA != 1.0f;

enum
{
    K_ONE,
    K_XC,
    K_U3,
    K_H0,
    K_H1,
    K_U2,
    K_S,
    K_U1,
    K_LC,
    K_CM,
    K_CB,
    K_LSB,
    K_HALF,
    K_G,
    K_LB,
    K_PM0,
    K_PM1,
    K_PM2,
    K_PM3,
    K_PB1,
    K_PB2,
    K_PB3,
    K_H0B,
    K_H1B,
    K_BASE_COUNT,
    // The curve stream: the tone curve, then copies of the entries from K_ONE to K_LB in the order in
    // which the kernel with the curve reads them.
    K_CT8 = K_BASE_COUNT,
    K_CM8,
    K_CT4,
    K_CM4,
    K_CT2,
    K_CM2,
    K_CF,
    K_CD8,
    K_CONE,
    K_C4,
    K_CD4,
    K_C3,
    K_CD2,
    K_C2,
    K_C1,
    K_C0,
    K_EONE,
    K_EXC,
    K_EU3,
    K_EU2,
    K_EU1,
    K_ES,
    K_ELC,
    K_ECM,
    K_ECB,
    K_EG,
    K_ELB,
    K_EH0,
    K_EH1,
    K_ELSB,
    K_EHALF,
    K_CURVE_END,
    K_COUNT = CURVE ? K_CURVE_END : K_BASE_COUNT
};

inline int16_t sat(int32_t v)
{
    return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
inline int16_t adds(int16_t x, int16_t y)
{
    return sat(static_cast<int32_t>(x) + y);
}
inline int16_t subs(int16_t x, int16_t y)
{
    return sat(static_cast<int32_t>(x) - y);
}
inline int16_t mul(int16_t x, int16_t y, int s)
{
    return static_cast<int16_t>(static_cast<uint16_t>((static_cast<int32_t>(x) * y) >> s));
}
inline int16_t vmin(int16_t x, int16_t y)
{
    return x <= y ? x : y;
}
inline int16_t vmax(int16_t x, int16_t y)
{
    return x >= y ? x : y;
}
inline int16_t lt(int16_t x, int16_t y)
{
    return x < y ? -1 : 0;
}
inline int16_t gt(int16_t x, int16_t y)
{
    return x > y ? -1 : 0;
}
inline int16_t vand(int16_t x, int16_t y)
{
    return static_cast<int16_t>(x & y);
}

// The tone curve, L0^(1 / TONEMAP_GAMMA) for L0 from 0 to 32767. Three steps for s = 8, 4 and 2
// take x = L0 into 8192 to 32767, 1/4 to 1, for every L0 above 0: where x is below CT = 2^(15 - s)
// it becomes x + x * CM = x * 2^s. The product at shift 0 wraps in the lanes that do not take it. In
// the same lanes f loses f * CD, which multiplies it by 2^(-s / TONEMAP_GAMMA). p is x^(1 /
// TONEMAP_GAMMA) on 1/4 to 1, a polynomial of degree 4 in u = x - 32767 with p = C0 = 32767 at u = 0,
// and the curve is -(p * f). f starts at CF = -1, which q15 holds exactly, so 32767 maps to 32767.
// f is 0 for L0 = 0, so 0 maps to 0.
inline int16_t curve(const int16_t* k, int16_t L0)
{
    const int16_t m8 = lt(L0, k[K_CT8]);
    int16_t       x  = adds(L0, vand(mul(L0, k[K_CM8], 0), m8));
    const int16_t m4 = lt(x, k[K_CT4]);
    x                = adds(x, vand(mul(x, k[K_CM4], 0), m4));
    const int16_t m2 = lt(x, k[K_CT2]);
    x                = adds(x, vand(mul(x, k[K_CM2], 0), m2));
    int16_t f        = vand(gt(L0, 0), k[K_CF]);
    f                = subs(f, vand(mul(f, k[K_CD8], 15), m8));
    f                = subs(f, vand(mul(f, k[K_CD4], 15), m4));
    f                = subs(f, vand(mul(f, k[K_CD2], 15), m2));
    const int16_t u  = subs(x, k[K_CONE]);
    int16_t       p  = adds(mul(k[K_C4], u, 15), k[K_C3]);
    p                = adds(mul(p, u, 15), k[K_C2]);
    p                = adds(mul(p, u, 15), k[K_C1]);
    p                = adds(mul(p, u, 15), k[K_C0]);
    return subs(0, mul(p, f, 15));
}

inline int16_t img_edge(const int16_t* k, int16_t L0)
{
    const int16_t lo = mul(L0, k[K_S], 15);
    const int16_t ch = adds(mul(vmin(L0, k[K_LC]), k[K_CM], 15), k[K_CB]);
    const int16_t x  = vmin(subs(k[K_ONE], L0), k[K_XC]);
    int16_t       t  = adds(mul(x, k[K_U3], 15), k[K_U2]);
    t                = adds(mul(x, t, 15), k[K_U1]);
    const int16_t up = mul(x, t, 15);
    return vmin(vmin(lo, ch), up);
}

inline int16_t pan_edge(const int16_t* k, int16_t L2)
{
    int16_t p = mul(L2, k[K_PM0], 15);
    p         = vmin(p, adds(mul(L2, k[K_PM1], 15), k[K_PB1]));
    p         = vmin(p, adds(mul(L2, k[K_PM2], 15), k[K_PB2]));
    return vmin(p, adds(mul(L2, k[K_PM3], 15), k[K_PB3]));
}
}

const int16_t* tone_fx_squeeze_params();
