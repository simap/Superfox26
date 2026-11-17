// The parameters of tone_fx_squeeze() and the lane steps that its fit and its stand in share. For
// tone_fx_squeeze.cpp, the stand in (host_tests/tone_fx/model), the fit (host_tests/tone_tables)
// and the tests only. The steps and
// their fixed point form are described in tone_fx_squeeze.cpp.
#pragma once

#include <cstdint>

// Quotient bits of the share. Each bit is 6 vector operations. The share error is below
// 2^-DIV_BITS, the chroma error below pan * 2^-DIV_BITS, 0.00035 at 9.
#define SQZ_DIV_BITS 9

namespace tone_fx_squeeze_internal
{
const int DIV_BITS = SQZ_DIV_BITS;

// The parameters, int16, in the order the vector kernel loads them. Some appear twice because the
// kernel reloads them. Every value is q15 except K_LSB, the integer 1.
enum
{
    K_ONE,  // 32767, the real 1 (tone_fx.h)
    K_XC,   // 1 - the left end of the table step that holds sRGB red
    K_U3,   // cubic of the upper branch, x^3
    K_H0,   // hue0, the a of sRGB red's unit hue vector
    K_H1,   // hue1, its b
    K_U2,   // cubic, x^2
    K_S,    // slope of the ray from black through sRGB red, red_c / red_l
    K_U1,   // cubic, x
    K_LC,   // right end of the step that holds sRGB red
    K_CM,   // slope of the table across that step
    K_CB,   // and its value at L = 0
    K_LSB,  // 1, the smallest image edge, so the divide never sees 0
    K_HALF, // 16384, the real 0.5
    K_G,    // TONEMAP_COMPRESS * (cusp_l - red_l) / red_c
    K_LB,   // L of the black entry
    K_PM0,  // panel line 0, slope. It passes through black, so it has no intercept.
    K_PM1,  // panel line 1, slope
    K_PM2,
    K_PM3,
    K_PB1, // panel line 1, value at L = 0
    K_PB2,
    K_PB3,
    K_H0B, // hue0 again
    K_H1B, // hue1 again
    K_COUNT
};

// The operations of the vector unit, one lane.
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

// The image edge at L0 >= 0, before the floor at 1.
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

// The panel edge at L2.
inline int16_t pan_edge(const int16_t* k, int16_t L2)
{
    int16_t p = mul(L2, k[K_PM0], 15);
    p         = vmin(p, adds(mul(L2, k[K_PM1], 15), k[K_PB1]));
    p         = vmin(p, adds(mul(L2, k[K_PM2], 15), k[K_PB2]));
    return vmin(p, adds(mul(L2, k[K_PM3], 15), k[K_PB3]));
}
} // namespace tone_fx_squeeze_internal

// The parameters, K_COUNT of them in the order above, as the kernel reads them. tone_fx_squeeze.cpp
// defines it on main/tone_tables.inc, and the generator of that file on the parameters it fits.
const int16_t* tone_fx_squeeze_params();
