// Definitions the squeeze tone effect shares between its coefficient table and the code that
// applies it.
// The K enum names each coefficient by its position in the parameter array, so the scalar functions
// here and the vector kernel in the source file read the same values in one order.
// The order of the K enum is the layout of the parameter array that tone_fx_squeeze_params returns.
// The table in tone_tables and the coefficient loads in the kernel must both follow it.
#pragma once

#include <cstdint>

#define SQZ_DIV_BITS 9

namespace tone_fx_squeeze_internal
{
const int DIV_BITS = SQZ_DIV_BITS;

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
    K_COUNT
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
