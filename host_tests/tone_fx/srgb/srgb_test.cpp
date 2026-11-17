// Host test of the sRGB decode, main/tone_fx_srgb.h. Run from this directory: make test.
//
// 1. The exact table of ../model (tone_fx_srgb_table) against the decode in long double, rounded to
//    nearest and held to 32767.
// 2. tone_fx_srgb_byte, the arithmetic of every lane of the kernel, against that table at all 256
//    bytes: the worst error, the count at each error size, and that the output never decreases.
// 3. tone_fx_srgb_to_lin, as its stand in, and tone_fx_srgb_to_lin_lut on interleaved pixels against
//    the per byte values.
// 4. The instruction sequence of main/tone_fx_srgb.cpp on a model of the vector instructions
//    (pie_emu.h) against the stand in, at every source alignment 0 to 15 and at pixel counts with
//    and without an 8 pixel tail. The bytes after the last pixel hold guard values. The sequence
//    must load no block past the one that holds byte 3 n + 15, none before the one that holds byte
//    0, and must leave every output past n as it was.
// Exit status 1 when the worst error is above 2 counts, the output decreases anywhere, or any
// check differs.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pie_emu.h"
#include "tone_fx_model.h"
#include "tone_fx_srgb.h"
#include "tone_fx_srgb_internal.h"

namespace
{
using namespace tone_fx_srgb_internal;

long double curve(int v)
{
    const long double c = v / 255.0L;
    return c <= 0.04045L ? c / 12.92L : powl((c + 0.055L) / 1.055L, 2.4L);
}

alignas(16) const uint8_t x80[16] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                     0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
alignas(16) const int16_t kc[8]   = {K, CLAMP, C5, C4, C3, C2, C1, C0};

// The 4 out shuffle steps on (q3, q4, q5), the result in (q3, q1, q4).
void shuffle_emu(pie& e)
{
    e.orq(1, 4, 4);
    e.srci_2q(5, 1, 7);
    e.vzip8(3, 1);
    e.vzip8(4, 5);
    e.orq(5, 1, 1);
    e.srci_2q(4, 5, 7);
    e.vzip8(3, 5);
    e.vzip8(1, 4);
    e.orq(4, 5, 5);
    e.srci_2q(1, 4, 7);
    e.vzip8(3, 4);
    e.vzip8(5, 1);
    e.orq(1, 4, 4);
    e.srci_2q(5, 1, 7);
    e.vzip8(3, 1);
    e.vzip8(4, 5);
}

// deinterleave_pie of tone_fx_srgb.cpp, instruction for instruction.
void deinterleave_emu(pie& e, const uint8_t* src, int blocks, q15* r, q15* g, q15* b)
{
    const uint8_t* k = x80;
    e.ld_usar(0, src, 16);
    e.vld(7, k, 0);
    do
    {
        e.vld(1, src, 16);
        e.vld(2, src, 16);
        e.src_q(3, 0, 1);
        e.vld(0, src, 16);
        e.src_q(4, 1, 2);
        e.xorq(3, 3, 7);
        e.src_q(5, 2, 0);
        e.xorq(4, 4, 7);
        e.xorq(5, 5, 7);
        shuffle_emu(e);
        e.zero(2);
        e.zero(5);
        e.zero(6);
        e.vzip8(2, 3);
        e.vzip8(5, 1);
        e.vzip8(6, 4);
        e.vst(2, r, 16);
        e.vst(3, r, 16);
        e.vst(5, g, 16);
        e.vst(1, g, 16);
        e.vst(6, b, 16);
        e.vst(4, b, 16);
    } while(--blocks);
}

// deinterleave8_pie of tone_fx_srgb.cpp, instruction for instruction.
void deinterleave8_emu(pie& e, const uint8_t* src, q15* r, q15* g, q15* b)
{
    const uint8_t* k    = x80;
    const uint8_t* last = src + 23;
    e.ld_usar(0, src, 16);
    e.vld(1, src, 0);
    e.vld(2, last, 0);
    e.vld(7, k, 0);
    e.src_q(3, 0, 1);
    e.src_q(4, 1, 2);
    e.zero(5);
    e.xorq(3, 3, 7);
    e.xorq(4, 4, 7);
    shuffle_emu(e);
    e.zero(2);
    e.zero(5);
    e.zero(6);
    e.vzip8(2, 3);
    e.vzip8(5, 1);
    e.vzip8(6, 4);
    e.vst(2, r, 0);
    e.vst(5, g, 0);
    e.vst(6, b, 0);
}

// curve_pie of tone_fx_srgb.cpp, instruction for instruction.
void curve_emu(pie& e, q15* a, int pairs)
{
    const int16_t* k = kc;
    q15*           o = a;
    e.sar            = 15;
    do
    {
        e.vld(0, a, 16);
        e.vld(1, a, 16);
        e.vldbc16(6, k, 2);
        e.vldbc16(7, k, 2);
        e.vmul16(2, 0, 6);
        e.vmul16(3, 1, 6);
        e.vmax16(0, 0, 7);
        e.vmax16(1, 1, 7);
        e.vldbc16(6, k, 2);
        e.vldbc16(7, k, 2);
        e.vmul16(4, 0, 6);
        e.vmul16(5, 1, 6);
        e.vldbc16(6, k, 2);
        e.vadds16(4, 4, 7);
        e.vadds16(5, 5, 7);
        e.vmul16(4, 4, 0);
        e.vmul16(5, 5, 1);
        e.vldbc16(7, k, 2);
        e.sar = 19;
        e.vadds16(4, 4, 6);
        e.vadds16(5, 5, 6);
        e.vmul16(4, 4, 0);
        e.vmul16(5, 5, 1);
        e.vldbc16(6, k, 2);
        e.sar = 15;
        e.vadds16(4, 4, 7);
        e.vadds16(5, 5, 7);
        e.vmul16(4, 4, 0);
        e.vmul16(5, 5, 1);
        e.vldbc16(7, k, 2);
        k -= 8;
        e.vadds16(4, 4, 6);
        e.vadds16(5, 5, 6);
        e.vmul16(4, 4, 0);
        e.vmul16(5, 5, 1);
        e.vadds16(4, 4, 2);
        e.vadds16(5, 5, 3);
        e.vadds16(4, 4, 7);
        e.vadds16(5, 5, 7);
        e.vst(4, o, 16);
        e.vst(5, o, 16);
    } while(--pairs);
}

// curve8_pie of tone_fx_srgb.cpp, instruction for instruction.
void curve8_emu(pie& e, q15* a)
{
    const int16_t* k = kc;
    e.sar            = 15;
    e.vld(0, a, 0);
    e.vldbc16(6, k, 2);
    e.vldbc16(7, k, 2);
    e.vmul16(2, 0, 6);
    e.vmax16(0, 0, 7);
    e.vldbc16(6, k, 2);
    e.vldbc16(7, k, 2);
    e.vmul16(4, 0, 6);
    e.vldbc16(6, k, 2);
    e.vadds16(4, 4, 7);
    e.vmul16(4, 4, 0);
    e.vldbc16(7, k, 2);
    e.sar = 19;
    e.vadds16(4, 4, 6);
    e.vmul16(4, 4, 0);
    e.vldbc16(6, k, 2);
    e.sar = 15;
    e.vadds16(4, 4, 7);
    e.vmul16(4, 4, 0);
    e.vldbc16(7, k, 2);
    e.vadds16(4, 4, 6);
    e.vmul16(4, 4, 0);
    e.vadds16(4, 4, 2);
    e.vadds16(4, 4, 7);
    e.vst(4, a, 0);
}

void curve_all_emu(pie& e, q15* a, int n)
{
    if(n >= 16) curve_emu(e, a, n / 16);
    if(n % 16) curve8_emu(e, a + n - 8);
}

// tone_fx_srgb_to_lin of tone_fx_srgb.cpp with the passes above.
void to_lin_emu(pie& e, const uint8_t* rgb, int n, q15* r, q15* g, q15* b)
{
    const int blocks = n / 16;
    if(blocks > 0) deinterleave_emu(e, rgb, blocks, r, g, b);
    if(n % 16)
    {
        const int done = 16 * blocks;
        deinterleave8_emu(e, rgb + 3 * done, r + done, g + done, b + done);
    }
    curve_all_emu(e, r, n);
    curve_all_emu(e, g, n);
    curve_all_emu(e, b, n);
}
} // namespace

int main()
{
    int fail = 0;

    // 1. The table.
    const q15*  table       = tone_fx_srgb_table();
    int         bad_table   = 0;
    long double nearest_tie = 1;
    for(int v = 0; v < 256; v++)
    {
        const long double s   = curve(v) * 32768.0L;
        const long double fl  = floorl(s + 0.5L);
        const int         ref = fl > 32767.0L ? 32767 : static_cast<int>(fl);
        const long double tie = fabsl(s - floorl(s) - 0.5L);
        if(tie < nearest_tie) nearest_tie = tie;
        if(table[v] != ref)
        {
            printf("table: byte %d holds %d, decode rounds to %d\n", v, table[v], ref);
            bad_table++;
        }
    }
    printf("table: %d of 256 entries differ from the rounded decode, closest fraction to a half %.6Lf\n",
           bad_table, nearest_tie);
    fail |= bad_table != 0;

    // 2. The calculated curve.
    int worst = 0, hist[8] = {0}, decreasing = 0, prev = -1;
    for(int v = 0; v < 256; v++)
    {
        const int o = tone_fx_srgb_byte(static_cast<uint8_t>(v));
        const int e = abs(o - table[v]);
        if(e > worst) worst = e;
        hist[e < 7 ? e : 7]++;
        if(o < prev)
        {
            printf("curve: byte %d gives %d, below byte %d's %d\n", v, o, v - 1, prev);
            decreasing++;
        }
        prev = o;
    }
    printf("curve: worst error %d counts of q15 over 256 bytes, %d decreasing steps\n", worst, decreasing);
    printf("curve: bytes at error 0: %d, 1: %d, 2: %d, 3 to 6: %d, 7 or more: %d\n", hist[0], hist[1], hist[2],
           hist[3] + hist[4] + hist[5] + hist[6], hist[7]);
    printf("curve: dark end, byte: table / calculated:");
    for(int v = 0; v <= 12; v++) printf(" %d: %d / %d", v, table[v], tone_fx_srgb_byte(static_cast<uint8_t>(v)));
    printf("\n");
    printf("curve: byte 0 gives %d, byte 255 gives %d\n", tone_fx_srgb_byte(0), tone_fx_srgb_byte(255));
    fail |= worst > 2 || decreasing != 0 || tone_fx_srgb_byte(0) != 0 || tone_fx_srgb_byte(255) != Q15_ONE;

    // 3 and 4. The array forms. Pixel i holds bytes i, i + 85, i + 170 mod 256. The bytes after the
    // last pixel are guard values from a hash, other ones per call.
    const int              NP = 256;
    alignas(16) uint8_t    src[3 * NP + 48];
    alignas(16) static q15 out_c[3][NP], out_l[3][NP], out_e[3][NP];
    static const int       counts[] = {NP, NP - 8, 8, 16, 24, 40, 48};
    int                    differ_c = 0, differ_l = 0, differ_e = 0, written_e = 0, reads_out = 0;
    uint32_t               h        = 1;
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
            for(uint8_t* p = rgb + 3 * n; p < src + sizeof(src); p++)
            {
                h  = h * 1103515245u + 12345u;
                *p = static_cast<uint8_t>(h >> 16);
            }
            memset(out_c, 0x55, sizeof(out_c));
            memset(out_l, 0x55, sizeof(out_l));
            memset(out_e, 0x55, sizeof(out_e));
            tone_fx_srgb_to_lin(rgb, n, out_c[0], out_c[1], out_c[2]);
            tone_fx_srgb_to_lin_lut(rgb, n, out_l[0], out_l[1], out_l[2]);
            pie e;
            e.win_lo = reinterpret_cast<uintptr_t>(src);
            e.win_hi = reinterpret_cast<uintptr_t>(src + sizeof(src));
            to_lin_emu(e, rgb, n, out_e[0], out_e[1], out_e[2]);
            // The highest block loaded starts at or below byte 3 n, so it ends before byte 3 n + 16.
            const uintptr_t first = reinterpret_cast<uintptr_t>(rgb) & ~static_cast<uintptr_t>(15);
            if(e.rd_lo < first || e.rd_hi > reinterpret_cast<uintptr_t>(rgb + 3 * n))
            {
                if(reads_out < 8)
                    printf("reads: offset %d, %d pixels: blocks from %+ld to %+ld bytes from rgb\n", off, n,
                           static_cast<long>(e.rd_lo - reinterpret_cast<uintptr_t>(rgb)),
                           static_cast<long>(e.rd_hi - reinterpret_cast<uintptr_t>(rgb)));
                reads_out++;
            }
            for(int ch = 0; ch < 3; ch++)
                for(int i = 0; i < NP; i++)
                {
                    const bool in = i < n;
                    const int  v  = in ? rgb[3 * i + ch] : 0;
                    differ_c += in ? out_c[ch][i] != tone_fx_srgb_byte(static_cast<uint8_t>(v))
                                   : out_c[ch][i] != 0x5555;
                    differ_l += in ? out_l[ch][i] != table[v] : out_l[ch][i] != 0x5555;
                    differ_e += in && out_e[ch][i] != out_c[ch][i];
                    written_e += !in && out_e[ch][i] != 0x5555;
                }
        }
    }
    printf("arrays, 16 source alignments by %d pixel counts: the stand in differs from the per byte value at "
           "%d values, _lut from the table at %d, the vector sequence on the instruction model from the stand "
           "in at %d\n",
           static_cast<int>(sizeof(counts) / sizeof(counts[0])), differ_c, differ_l, differ_e);
    printf("vector sequence bounds: %d calls load a block outside rgb & ~15 to rgb + 3 n, %d outputs past n "
           "written\n",
           reads_out, written_e);
    fail |= differ_c != 0 || differ_l != 0 || differ_e != 0 || reads_out != 0 || written_e != 0;

    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
