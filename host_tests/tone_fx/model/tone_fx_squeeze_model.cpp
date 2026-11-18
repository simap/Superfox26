// The stand in of tone_fx_squeeze() (main/tone_fx_squeeze.h) for host builds and the probes. It runs
// the vector operations of main/tone_fx_squeeze.cpp on each colour, with the lane steps and the
// parameters of main/tone_fx_squeeze_internal.h, so it gives the same bits.

#include "tone_fx_model.h"
#include "tone_fx_squeeze.h"
#include "tone_fx_squeeze_internal.h"

namespace
{
using namespace tone_fx_squeeze_internal;

inline int16_t vmax(int16_t x, int16_t y)
{
    return x >= y ? x : y;
}
inline int16_t gt(int16_t x, int16_t y)
{
    return x > y ? -1 : 0;
}
inline int16_t vand(int16_t x, int16_t y)
{
    return static_cast<int16_t>(x & y);
}

// One colour, the operations of the vector kernel. The kernel runs them in another order, with the
// same results.
inline void squeeze_one(const int16_t* k, int16_t L, int16_t A, int16_t B, q15* lo, q15* ao, q15* bo, q15* co)
{
    const int16_t L0 = vmax(L, 0);
    const int16_t c  = vmax(adds(mul(A, k[K_H0], 15), mul(B, k[K_H1], 15)), 0);
    const int16_t e0 = img_edge(k, L0);
    const int16_t e  = vmax(e0, k[K_LSB]);
    const int16_t cm = vmin(c, e);
    const int16_t hm = mul(subs(e, k[K_LSB]), k[K_HALF], 15); // floor((e - 1) / 2), r > hm is 2r >= e
    const int16_t L2 = vmax(adds(L0, mul(e0, k[K_G], 15)), 0);
    *lo              = vmax(L2, k[K_LB]);
    // Restoring division, q = floor(cm / e * 2^DIV_BITS). r <= e after every bit, 2r <= 2 * red_c.
    int16_t r = cm, q = 0;
    for(int i = 0; i < DIV_BITS; i++)
    {
        const int16_t m = gt(r, hm);
        r               = adds(r, r);
        q               = adds(q, q);
        r               = subs(r, vand(m, e));
        q               = subs(q, m);
    }
    q                = subs(q, r == e ? -1 : 0);
    const int16_t p  = pan_edge(k, L2);
    const int16_t c2 = mul(q, p, DIV_BITS);
    *co              = c2;
    *ao              = mul(c2, k[K_H0B], 15);
    *bo              = mul(c2, k[K_H1B], 15);
}
} // namespace

void tone_fx_squeeze(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co)
{
    const int16_t* k = tone_fx_squeeze_params();
    for(int i = 0; i < n; i++) squeeze_one(k, L[i], A[i], B[i], &Lo[i], &Ao[i], &Bo[i], &Co[i]);
}
