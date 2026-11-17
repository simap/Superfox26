// The stand in of tone_fx_srgb_to_lin() (main/tone_fx_srgb.h) for host builds and the probes, and
// the exact decode table the tests measure against. The stand in runs the steps of each vector lane
// of main/tone_fx_srgb.cpp, so it gives the same bits. It reads no byte past the last pixel.

#include "tone_fx_model.h"
#include "tone_fx_srgb.h"
#include "tone_fx_srgb_internal.h"

namespace
{
using namespace tone_fx_srgb_internal;

// The decode of every byte, rounded to nearest and held to 32767 (host_tests/tone_fx/srgb/fit.cpp
// prints it, srgb_test.cpp checks it against long double).
const q15 k_table[256] = {
    0,     10,    20,    30,    40,    50,    60,    70,    80,    90,    99,    110,   120,   132,   144,
    157,   170,   184,   198,   213,   229,   246,   263,   281,   299,   319,   338,   359,   381,   403,
    425,   449,   473,   498,   524,   551,   578,   606,   635,   665,   695,   727,   759,   792,   825,
    860,   895,   931,   969,   1006,  1045,  1085,  1125,  1167,  1209,  1252,  1296,  1341,  1386,  1433,
    1481,  1529,  1578,  1629,  1680,  1732,  1785,  1839,  1894,  1950,  2007,  2065,  2123,  2183,  2244,
    2306,  2368,  2432,  2496,  2562,  2629,  2696,  2765,  2834,  2905,  2977,  3049,  3123,  3198,  3273,
    3350,  3428,  3507,  3587,  3668,  3750,  3833,  3917,  4002,  4089,  4176,  4264,  4354,  4444,  4536,
    4629,  4723,  4818,  4914,  5011,  5109,  5209,  5309,  5411,  5514,  5618,  5723,  5829,  5936,  6045,
    6155,  6265,  6377,  6490,  6605,  6720,  6837,  6954,  7073,  7193,  7315,  7437,  7561,  7686,  7812,
    7939,  8068,  8197,  8328,  8460,  8593,  8728,  8864,  9001,  9139,  9278,  9419,  9561,  9704,  9848,
    9994,  10141, 10289, 10438, 10589, 10741, 10894, 11048, 11204, 11361, 11519, 11679, 11839, 12001, 12165,
    12329, 12495, 12663, 12831, 13001, 13172, 13344, 13518, 13693, 13870, 14047, 14226, 14407, 14588, 14771,
    14956, 15141, 15328, 15517, 15706, 15897, 16090, 16284, 16479, 16675, 16873, 17072, 17273, 17474, 17678,
    17882, 18088, 18296, 18504, 18715, 18926, 19139, 19353, 19569, 19786, 20005, 20225, 20446, 20669, 20893,
    21118, 21345, 21574, 21803, 22035, 22267, 22501, 22737, 22974, 23212, 23452, 23693, 23936, 24180, 24425,
    24672, 24921, 25171, 25422, 25675, 25929, 26185, 26442, 26701, 26961, 27223, 27486, 27750, 28016, 28284,
    28553, 28823, 29095, 29369, 29644, 29920, 30198, 30478, 30759, 31041, 31325, 31611, 31898, 32186, 32476,
    32767,
};

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

void tone_fx_srgb_to_lin_lut(const uint8_t* rgb, int n, q15* r, q15* g, q15* b)
{
    for(int i = 0; i < n; i++, rgb += 3) r[i] = k_table[rgb[0]], g[i] = k_table[rgb[1]], b[i] = k_table[rgb[2]];
}

q15 tone_fx_srgb_byte(uint8_t v)
{
    return byte_c(v);
}

const q15* tone_fx_srgb_table()
{
    return k_table;
}
