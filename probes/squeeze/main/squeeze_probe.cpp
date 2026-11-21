// Self test of the squeeze kernel against its stand in, which this build names
// tone_fx_squeeze_model, on fixed and pseudo random inputs and then on every 16 bit L. The last line
// is SQUEEZE PROBE DONE.
#include <cstdio>

#include "esp_heap_caps.h"
#include "tone_fx_model.h"
#include "tone_fx_squeeze.h"
#include "tone_tables.h"

namespace
{
// xorshift32, for the fixed test inputs.
uint32_t next_rand(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

template <typename T> T* alloc16(int n)
{
    return static_cast<T*>(heap_caps_aligned_alloc(16, sizeof(T) * static_cast<size_t>(n), MALLOC_CAP_8BIT));
}

// The kernel and its stand in over every combination of the edge values, then pseudo random values
// over the whole q15 range, out of place and in place. Returns the number of output elements that
// differ, -1 when a buffer could not be allocated.
int squeeze_selftest()
{
    static const int16_t ev_l[] = {-32768, -16384, -1,    0,     1,     2,     100,   127,   128,
                                   2000,   2047,   2048,  8000,  8191,  8192,  13000, 16384, 20000,
                                   20577,  21000,  24000, 30000, 32000, 32766, 32767};
    static const int16_t ev_c[] = {-32768, -20000, -300, -1, 0, 1, 2, 30, 300, 3000, 8000, 32767};
    const int            NE     = sizeof(ev_l) / sizeof(ev_l[0]) * (sizeof(ev_c) / sizeof(ev_c[0])) *
                         (sizeof(ev_c) / sizeof(ev_c[0]));
    const int  NR = 4096;
    const int  N  = tone_fx_pad8(NE + NR);
    q15*       in = alloc16<q15>(3 * N);
    q15*       oc = alloc16<q15>(4 * N);
    q15*       op = alloc16<q15>(4 * N);
    if(!in || !oc || !op)
    {
        heap_caps_free(in);
        heap_caps_free(oc);
        heap_caps_free(op);
        return -1;
    }
    int i = 0;
    for(int16_t l : ev_l)
        for(int16_t a : ev_c)
            for(int16_t b : ev_c)
            {
                in[i] = l, in[N + i] = a, in[2 * N + i] = b;
                i++;
            }
    uint32_t seed = 0x5eed1234u;
    for(; i < N; i++)
    {
        in[i]         = static_cast<int16_t>(next_rand(&seed));
        in[N + i]     = static_cast<int16_t>(next_rand(&seed));
        in[2 * N + i] = static_cast<int16_t>(next_rand(&seed));
    }
    tone_fx_squeeze_model(in, in + N, in + 2 * N, N, oc, oc + N, oc + 2 * N, oc + 3 * N);
    tone_fx_squeeze(in, in + N, in + 2 * N, N, op, op + N, op + 2 * N, op + 3 * N);
    int diff = 0;
    for(int j = 0; j < 4 * N; j++) diff += oc[j] != op[j];
    // In place, L, a and b become Lo, Ao and Bo.
    tone_fx_squeeze(in, in + N, in + 2 * N, N, in, in + N, in + 2 * N, op + 3 * N);
    for(int j = 0; j < 3 * N; j++) diff += oc[j] != in[j];
    for(int j = 3 * N; j < 4 * N; j++) diff += oc[j] != op[j];
    heap_caps_free(in);
    heap_caps_free(oc);
    heap_caps_free(op);
    return diff;
}

// The kernel and its stand in at every 16 bit L, in blocks, with pseudo random a and b. Returns the
// number of output elements that differ, -1 when a buffer could not be allocated.
int squeeze_every_l()
{
    const int N  = 2048;
    q15*      in = alloc16<q15>(3 * N);
    q15*      oc = alloc16<q15>(4 * N);
    q15*      op = alloc16<q15>(4 * N);
    int       diff = 0;
    if(!in || !oc || !op) diff = -1;
    uint32_t seed = 0x0dd5eed5u;
    for(int l0 = -32768; diff >= 0 && l0 < 32768; l0 += N)
    {
        for(int i = 0; i < N; i++)
        {
            in[i]         = static_cast<int16_t>(l0 + i);
            in[N + i]     = static_cast<int16_t>(next_rand(&seed));
            in[2 * N + i] = static_cast<int16_t>(next_rand(&seed));
        }
        tone_fx_squeeze_model(in, in + N, in + 2 * N, N, oc, oc + N, oc + 2 * N, oc + 3 * N);
        tone_fx_squeeze(in, in + N, in + 2 * N, N, op, op + N, op + 2 * N, op + 3 * N);
        for(int j = 0; j < 4 * N; j++) diff += oc[j] != op[j];
    }
    heap_caps_free(in);
    heap_caps_free(oc);
    heap_caps_free(op);
    return diff;
}
} // namespace

extern "C" void app_main(void)
{
    printf("SQUEEZE parameters of main/tone_tables.inc, edge fit error image %.6f panel %.6f, tone curve at gamma %g "
           "error %.6f\n",
           static_cast<double>(tone_tables::squeeze_fit_img), static_cast<double>(tone_tables::squeeze_fit_pan),
           static_cast<double>(TONEMAP_GAMMA), static_cast<double>(tone_tables::squeeze_fit_curve));
    const int diff = squeeze_selftest();
    printf("SQUEEZE selftest: %d elements differ between the kernel and its stand in\n", diff);
    const int diff_l = squeeze_every_l();
    printf("SQUEEZE every L: %d elements differ between the kernel and its stand in\n", diff_l);
    printf("SQUEEZE PROBE DONE\n");
}
