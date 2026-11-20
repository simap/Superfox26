// Probe of the tone_fx srgb and bracket kernels. Each kernel of main/ runs beside its stand in of
// host_tests/tone_fx/model, which this build names *_model. At boot it prints, each line once:
//   VMUL: ee.vmul.s16 on edge lanes against the arithmetic shift and low 16 bits the kernels
//         assume, per lane, then a count of lanes that differ.
//   SELFTEST srgb: the outputs where the kernel differs from its stand in, and the outputs past n it
//         wrote, with guard bytes after the input.
//   ACCURACY srgb: the kernel against the exact decode at all 256 bytes.
//   SELFTEST bracket: the outputs where the kernel differs from its stand in.
//   PROBE DONE, last.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "esp_heap_caps.h"
#include "tone_fx_bracket.h"
#include "tone_fx_bracket_internal.h"
#include "tone_fx_model.h"
#include "tone_fx_srgb.h"

namespace
{
// ee.vmul.s16 of a and b with SAR s, as the kernels assume it: the 32 bit product shifted right
// arithmetically, low 16 bits kept.
int16_t assumed(int16_t a, int16_t b, int s)
{
    return static_cast<int16_t>(static_cast<uint16_t>((static_cast<int32_t>(a) * b) >> s));
}

int vmul_check()
{
    // Lanes: negative odd products (floor against round toward zero), products that do not fit 16
    // bits after the shift (wrap against saturate), and in range products.
    alignas(16) static const int16_t a[2][8] = {{-3, 3, -1, 32767, -32768, 300, -300, 181},
                                                {-1, 1, -32768, -32768, 16384, -16385, 12345, -32767}};
    alignas(16) static const int16_t b[2][8] = {{1, 1, 1, 32767, -32768, 300, 300, 181},
                                                {1, 1, 1, -32768, 16384, 16385, -12345, -32767}};
    static const int                 sar[2]  = {1, 15};
    int                              differ  = 0;
    for(int set = 0; set < 2; set++)
    {
        alignas(16) int16_t out[8];
        const int16_t*      pa = a[set];
        const int16_t*      pb = b[set];
        int16_t*            po = out;
        __asm__ volatile("wsr.sar           %[s]           \n"
                         "ee.vld.128.ip     q0, %[pa], 0   \n"
                         "ee.vld.128.ip     q1, %[pb], 0   \n"
                         "ee.vmul.s16       q2, q0, q1     \n"
                         "ee.vst.128.ip     q2, %[po], 0   \n"
                         : [pa] "+r"(pa), [pb] "+r"(pb), [po] "+r"(po)
                         : [s] "r"(sar[set])
                         : "memory");
        for(int i = 0; i < 8; i++)
        {
            const int16_t want = assumed(a[set][i], b[set][i], sar[set]);
            printf("VMUL sar %2d: %6d * %6d -> %6d, assumed %6d%s\n", sar[set], a[set][i], b[set][i], out[i], want,
                   out[i] == want ? "" : "  DIFFERS");
            differ += out[i] != want;
        }
    }
    printf("VMUL: %d of 16 lanes differ from the arithmetic shift and low 16 bits\n", differ);
    return differ;
}

// The kernel and its stand in on every byte value in each of R, G and B: pixel i holds bytes i,
// i + 85 and i + 170 mod 256. The source sits at each offset 0 to 15 from a 16 byte boundary, at
// pixel counts with and without an 8 pixel tail. The 16 bytes after the last pixel hold guard values
// that change from call to call. Returns the outputs below n that differ, and in *written the
// outputs at n or above that the kernel wrote. -1 when a buffer could not be allocated.
int srgb_selftest(int* written)
{
    const int NP  = 256;
    uint8_t*  src = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, 3 * NP + 32, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    q15*      out = static_cast<q15*>(heap_caps_aligned_alloc(16, 6 * NP * sizeof(q15), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    *written      = 0;
    if(!src || !out)
    {
        heap_caps_free(src);
        heap_caps_free(out);
        return -1;
    }
    static const int counts[] = {NP, NP - 8, 8, 16, 24, 40};
    uint32_t         h        = 1;
    int              differ   = 0;
    for(int off = 0; off < 16; off++)
    {
        uint8_t* rgb = src + off;
        for(int n : counts)
        {
            for(int i = 0; i < n; i++)
            {
                rgb[3 * i]     = static_cast<uint8_t>(i);
                rgb[3 * i + 1] = static_cast<uint8_t>(i + 85);
                rgb[3 * i + 2] = static_cast<uint8_t>(i + 170);
            }
            for(int i = 3 * n; i < 3 * n + 16; i++)
            {
                h      = h * 1103515245u + 12345u;
                rgb[i] = static_cast<uint8_t>(h >> 16);
            }
            memset(out, 0x55, 6 * NP * sizeof(q15));
            q15* p = out;
            q15* c = out + 3 * NP;
            tone_fx_srgb_to_lin(rgb, n, p, p + NP, p + 2 * NP);
            tone_fx_srgb_to_lin_model(rgb, n, c, c + NP, c + 2 * NP);
            for(int ch = 0; ch < 3; ch++)
                for(int i = 0; i < NP; i++)
                {
                    if(i < n) differ += p[ch * NP + i] != c[ch * NP + i];
                    else *written += p[ch * NP + i] != 0x5555;
                }
        }
    }
    heap_caps_free(src);
    heap_caps_free(out);
    return differ;
}

// The decode of byte v in double, rounded to nearest and held to 32767.
int exact(int v)
{
    const double c  = v / 255.0;
    const double l  = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    const double fl = floor(l * 32768.0 + 0.5);
    return fl > 32767.0 ? 32767 : static_cast<int>(fl);
}

void srgb_accuracy()
{
    // 256 pixels, pixel i all three bytes i, and the 16 bytes the kernel may read after them.
    alignas(16) static uint8_t rgb[3 * 256 + 16];
    alignas(16) static q15     r[256], g[256], b[256];
    for(int i = 0; i < 256; i++) rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = static_cast<uint8_t>(i);
    tone_fx_srgb_to_lin(rgb, 256, r, g, b);
    int worst = 0, at[3] = {0};
    for(int i = 0; i < 256; i++)
        for(const q15* c : {r, g, b})
        {
            const int e = abs(c[i] - exact(i));
            if(e > worst) worst = e;
            at[e < 2 ? e : 2]++;
        }
    printf("ACCURACY srgb: the kernel against the exact decode, worst %d counts, values at 0: %d, 1: %d, 2 or more: "
           "%d, byte 0 -> %d, byte 255 -> %d\n",
           worst, at[0], at[1], at[2], r[0], r[255]);
}

// The kernel and its stand in on every int16 value of L, in chunks of CH. The last chunk runs as
// CH - 8 values, which ends in an 8 value tail, and then its last 8 values as a call of 8. Returns
// the number of lo and hi bytes that differ, -1 when a buffer could not be allocated.
int bracket_selftest()
{
    const int CH  = 512;
    q15*      L   = static_cast<q15*>(heap_caps_aligned_alloc(16, CH * sizeof(q15), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint8_t*  out = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, 4 * CH, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if(!L || !out)
    {
        heap_caps_free(L);
        heap_caps_free(out);
        return -1;
    }
    int differ = 0;
    for(int v0 = -32768; v0 < 32768; v0 += CH)
    {
        for(int i = 0; i < CH; i++) L[i] = static_cast<q15>(v0 + i);
        const bool last = v0 + CH == 32768;
        const int  n    = last ? CH - 8 : CH;
        tone_fx_bracket(L, n, out, out + CH);
        tone_fx_bracket_model(L, n, out + 2 * CH, out + 3 * CH);
        for(int i = 0; i < n; i++) differ += (out[i] != out[2 * CH + i]) + (out[CH + i] != out[3 * CH + i]);
        if(last)
        {
            tone_fx_bracket(L + n, 8, out, out + CH);
            tone_fx_bracket_model(L + n, 8, out + 2 * CH, out + 3 * CH);
            for(int i = 0; i < 8; i++) differ += (out[i] != out[2 * CH + i]) + (out[CH + i] != out[3 * CH + i]);
        }
    }
    heap_caps_free(L);
    heap_caps_free(out);
    return differ;
}
} // namespace

extern "C" void app_main()
{
    printf("tone_fx probe: srgb and bracket\n");
    vmul_check();
    int       written = 0;
    const int differ  = srgb_selftest(&written);
    printf("SELFTEST srgb: %d outputs differ, %d outputs past n written\n", differ, written);
    srgb_accuracy();
    printf("bracket table of main/tone_tables.inc: %d neutral entries, %d steps\n", tone_fx_neutral_count(),
           tone_fx_bracket_steps());
    printf("SELFTEST bracket: %d outputs differ\n", bracket_selftest());
    printf("PROBE DONE\n");
}
