// The stand in of tone_fx_srgb_to_lin() (main/tone_fx_srgb.h) for host builds and the probes. It
// runs the steps of each vector lane of main/tone_fx_srgb.cpp, so it gives the same bits. It reads no
// byte past the last pixel.

#include "tone_fx_model.h"
#include "tone_fx_srgb.h"
#include "tone_fx_srgb_internal.h"

namespace
{
using namespace tone_fx_srgb_internal;

// ee.vmul.s16 with SAR s: the product shifted right arithmetically, low 16 bits kept.
inline int16_t vmul(int16_t a, int16_t b, int s)
{
    return static_cast<int16_t>(static_cast<uint16_t>((static_cast<int32_t>(a) * b) >> s));
}

// ee.vadds.s16: the sum held to the int16 range.
inline int16_t vadds(int16_t a, int16_t b)
{
    const int32_t s = static_cast<int32_t>(a) + b;
    return static_cast<int16_t>(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

// The curve pass on one value t, as the vector form computes each lane.
inline q15 curve(int16_t t)
{
    const int16_t tc  = t > CLAMP ? t : CLAMP;
    const int16_t lin = vmul(t, K, 15);
    int16_t       h   = vadds(vmul(C5, tc, 15), C4);
    h                 = vadds(vmul(h, tc, 15), C3);
    h                 = vadds(vmul(h, tc, 19), C2);
    h                 = vadds(vmul(h, tc, 15), C1);
    h                 = vmul(h, tc, 15);
    return vadds(vadds(h, lin), C0);
}

inline q15 byte_c(uint8_t v)
{
    return curve(static_cast<int16_t>((v - 128) * 256));
}
} // namespace

void tone_fx_srgb_to_lin(const uint8_t* rgb, int n, q15* r, q15* g, q15* b)
{
    for(int i = 0; i < n; i++, rgb += 3)
    {
        r[i] = byte_c(rgb[0]);
        g[i] = byte_c(rgb[1]);
        b[i] = byte_c(rgb[2]);
    }
}

q15 tone_fx_srgb_byte(uint8_t v)
{
    return byte_c(v);
}
