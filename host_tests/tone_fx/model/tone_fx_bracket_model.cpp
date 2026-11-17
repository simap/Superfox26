// The stand in of tone_fx_bracket() (main/tone_fx_bracket.h) for host builds and the probes. It runs
// the steps of main/tone_fx_bracket.cpp on each value in the same order with the same saturating
// add, so it gives the same bits. It reads the step table through tone_fx_bracket_table().

#include "tone_fx_bracket.h"
#include "tone_fx_bracket_internal.h"
#include "tone_fx_model.h"

namespace
{
// ee.vadds.s16: the sum held to the int16 range.
inline int16_t vadds(int16_t a, int16_t b)
{
    const int32_t s = static_cast<int32_t>(a) + b;
    return static_cast<int16_t>(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}
} // namespace

void tone_fx_bracket(const q15* L, int n, uint8_t* lo, uint8_t* hi)
{
    const int16_t* table   = tone_fx_bracket_table();
    const int      n_steps = tone_fx_bracket_steps();
    const int16_t* steps   = table + 2;
    for(int i = 0; i < n; i++)
    {
        int16_t acc = table[0];
        for(int j = 0; j < n_steps; j++)
        {
            const int16_t mask = L[i] < steps[2 * j] ? -1 : 0;
            acc                = vadds(acc, static_cast<int16_t>(mask & steps[2 * j + 1]));
        }
        lo[i] = static_cast<uint8_t>(acc);
        hi[i] = static_cast<uint8_t>(static_cast<uint16_t>(acc) >> 8);
    }
}
