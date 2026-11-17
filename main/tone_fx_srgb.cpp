// sRGB bytes to linear light in q15. See tone_fx_srgb.h.
//
// The curve. t is the byte as q15 around byte 128: t = (v - 128) * 256, -1 at byte 0 and 0.992 at
// byte 255. tc is t held at or above CLAMP, the t of byte 11. The output is
//
//   P(tc) + K t
//
// P is a degree 5 polynomial fitted to the decode less K t over bytes 11 to 255
// (host_tests/tone_fx/srgb/fit.cpp). At or below byte 10 P(tc) is the constant P(CLAMP) and K t is
// the linear segment of the decode. K t in q15 is 10 (v - 128), exact at every byte. The constants
// are in tone_fx_srgb_internal.h.
//
// Arithmetic of each lane. Every multiply is the 16 bit product of two int16 shifted right
// arithmetically by SAR, so it rounds toward minus infinity, and the low 16 bits kept. No product
// here exceeds the int16 range. Every add saturates to the int16 range, and only the last add
// saturates, at byte 255. P is evaluated by Horner from C5 down: stages 5, 4 and 3 hold their value
// times 16 (q19), stages 2, 1 and 0 q15, so the multiply into stage 2 shifts by 19 and every other
// multiply by 15. The fit's integer search took the floors into account. K t is added before C0, so
// the sum stays in range until the last add. The stand in of the host tests runs the same steps.
//
// Per call: a deinterleave pass writes t of every pixel into r, g and b, then a curve pass rewrites
// each array in place.
//
// Deinterleave pass, 16 pixels (48 bytes) per loop. The source has any alignment. The loop loads
// aligned 16 byte blocks and ee.src.q shifts each pair of them by the source's low 4 address bits,
// which ee.ld.128.usar.ip puts in SAR_BYTE once before the loop. 48 bytes is 3 blocks, so that
// shift is the same for every loop. Three 16 byte vectors S0 S1 S2 then hold the 48 bytes in pixel
// order. One out shuffle step maps the 48 byte sequence S to Y with Y[2i] = S[i] and Y[2i + 1] =
// S[24 + i], i < 24: with S cut in 8 byte halves h0 to h5, Y is zip(h0, h3), zip(h1, h4),
// zip(h2, h5). The byte at position j goes to 2 j mod 47, so after 4 steps byte 3 m + c, pixel m
// channel c, is at 16 j mod 47 = 16 c + m: S0 holds the 16 R bytes, S1 the G bytes, S2 the B bytes.
// One step is 4 ops: a copy of S1, ee.srci.2q to cut (h3, h4) and (h5, 0) out of S1 and S2, and two
// ee.vzip.8. Before the steps every byte is XORed with 0x80, which makes it v - 128 as a signed
// byte. ee.vzip.8 of a zero vector and a byte vector puts each byte in the high half of a 16 bit
// lane, so the lanes hold t. Per 16 pixels: 3 loads, 3 ee.src.q, 3 XOR, 16 shuffle ops, 3 zero,
// 3 zip, 6 stores, 37 ops.
//
// When n is not a multiple of 16, the last 8 pixels (24 bytes) take the same steps with S2 zero.
// Pixels 0 to 7 of a block read bytes 3 m + c < 24 only, so no byte of S2 or of the high half of S1
// reaches them, and only their 8 values per array are stored.
//
// Curve pass, 16 values (2 vectors) per loop, the two vectors interleaved so no multiply result or
// load is used by the next op. The 8 constants are broadcast loads each loop. Per 16 values: 2
// loads, 8 constant loads, 2 max, 12 multiplies, 12 adds, 2 stores and 2 SAR writes, 40 ops. An
// odd last vector takes the same steps alone.
//
// Timing of the ESP32-S3 core the schedules assume: one instruction issues per cycle. A vector load
// or multiply result stalls a consumer placed right after it by one cycle and no consumer later.
// The other vector ops and the scalar adds forward to the next instruction. A taken branch costs
// about 3 cycles. Expected: about 11.2 cycles per pixel, the two passes below.

#include "tone_fx_srgb.h"

#if TONE_FX_PIE

#include "tone_fx_srgb_internal.h"

namespace
{
using namespace tone_fx_srgb_internal;

// 16 bytes of 0x80 for the XOR that makes each byte v - 128.
alignas(16) const uint8_t k_x80[16] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                       0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};

// The curve constants in the order the curve pass loads them.
alignas(16) const int16_t k_curve[8] = {K, CLAMP, C5, C4, C3, C2, C1, C0};

// The deinterleave pass over blocks of 16 pixels, blocks at least 1. Reads the aligned 16 byte
// blocks from the one holding src[0] up to the one holding src[48 * blocks]. Writes 16 values of t
// to each of r, g and b per block. Expected 42 cycles per 16 pixels, 2.6 per pixel: 39 instructions
// and the taken branch. Each load has one instruction between it and its first use, and the shuffle
// ops forward to each other.
void deinterleave_pie(const uint8_t* src, int blocks, q15* r, q15* g, q15* b)
{
    const uint8_t* k = k_x80;
    __asm__ volatile(
        "ee.ld.128.usar.ip q0, %[src], 16 \n" // q0 = block holding src[0], SAR_BYTE = src & 15
        "ee.vld.128.ip     q7, %[k], 0    \n" // q7 = 0x80 bytes
        "1:                               \n"
        "ee.vld.128.ip     q1, %[src], 16 \n"
        "ee.vld.128.ip     q2, %[src], 16 \n"
        "ee.src.q          q3, q0, q1     \n" // S0, pixel bytes 0 to 15
        "ee.vld.128.ip     q0, %[src], 16 \n" // the block the next loop starts from
        "ee.src.q          q4, q1, q2     \n" // S1, bytes 16 to 31
        "ee.xorq           q3, q3, q7     \n"
        "ee.src.q          q5, q2, q0     \n" // S2, bytes 32 to 47
        "ee.xorq           q4, q4, q7     \n"
        "ee.xorq           q5, q5, q7     \n"
        // Out shuffle step 1 on (q3, q4, q5), result in (q3, q1, q4).
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n" // q1 = (h3, h4), q5 = (h5, 0)
        "ee.vzip.8         q3, q1         \n" // q3 = zip(h0, h3), q1 = zip(h1, h4)
        "ee.vzip.8         q4, q5         \n" // q4 = zip(h2, h5)
        // Step 2 on (q3, q1, q4), result in (q3, q5, q1).
        "ee.orq            q5, q1, q1     \n"
        "ee.srci.2q        q4, q5, 7      \n"
        "ee.vzip.8         q3, q5         \n"
        "ee.vzip.8         q1, q4         \n"
        // Step 3 on (q3, q5, q1), result in (q3, q4, q5).
        "ee.orq            q4, q5, q5     \n"
        "ee.srci.2q        q1, q4, 7      \n"
        "ee.vzip.8         q3, q4         \n"
        "ee.vzip.8         q5, q1         \n"
        // Step 4 on (q3, q4, q5), result in (q3, q1, q4): R, G and B bytes.
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        // Widen: zero low byte, the byte high. Pixels 0 to 7 in the first vector, 8 to 15 in the
        // second.
        "ee.zero.q         q2             \n"
        "ee.zero.q         q5             \n"
        "ee.zero.q         q6             \n"
        "ee.vzip.8         q2, q3         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.vzip.8         q6, q4         \n"
        "ee.vst.128.ip     q2, %[r], 16   \n"
        "ee.vst.128.ip     q3, %[r], 16   \n"
        "ee.vst.128.ip     q5, %[g], 16   \n"
        "ee.vst.128.ip     q1, %[g], 16   \n"
        "ee.vst.128.ip     q6, %[b], 16   \n"
        "ee.vst.128.ip     q4, %[b], 16   \n"
        "addi              %[n], %[n], -1 \n"
        "bnez              %[n], 1b       \n"
        : [src] "+r"(src), [n] "+r"(blocks), [r] "+r"(r), [g] "+r"(g), [b] "+r"(b), [k] "+r"(k)
        :
        : "memory");
}

// The deinterleave of the 8 pixels (24 bytes) at src. Reads the aligned blocks holding src[0],
// src[16] and src[23], so no byte at or past src + 40. When src & 15 is 8 or less, src[23] lies in
// the block of src[16], and ee.src.q takes bytes 16 to 23 from that block alone. Writes 8 values of
// t to each of r, g and b. 34 instructions. Each load has one instruction between it and its first
// use.
void deinterleave8_pie(const uint8_t* src, q15* r, q15* g, q15* b)
{
    const uint8_t* k    = k_x80;
    const uint8_t* last = src + 23;
    __asm__ volatile(
        "ee.ld.128.usar.ip q0, %[src], 16 \n" // q0 = block holding src[0], SAR_BYTE = src & 15
        "ee.vld.128.ip     q1, %[src], 0  \n" // the block holding src[16]
        "ee.vld.128.ip     q2, %[last], 0 \n" // the block holding src[23]
        "ee.vld.128.ip     q7, %[k], 0    \n" // q7 = 0x80 bytes
        "ee.src.q          q3, q0, q1     \n" // S0, pixel bytes 0 to 15
        "ee.src.q          q4, q1, q2     \n" // S1, bytes 16 to 23 in its low half
        "ee.zero.q         q5             \n" // S2
        "ee.xorq           q3, q3, q7     \n"
        "ee.xorq           q4, q4, q7     \n"
        // The 4 out shuffle steps of deinterleave_pie.
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        "ee.orq            q5, q1, q1     \n"
        "ee.srci.2q        q4, q5, 7      \n"
        "ee.vzip.8         q3, q5         \n"
        "ee.vzip.8         q1, q4         \n"
        "ee.orq            q4, q5, q5     \n"
        "ee.srci.2q        q1, q4, 7      \n"
        "ee.vzip.8         q3, q4         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.orq            q1, q4, q4     \n"
        "ee.srci.2q        q5, q1, 7      \n"
        "ee.vzip.8         q3, q1         \n"
        "ee.vzip.8         q4, q5         \n"
        // Widen pixels 0 to 7 of R (q3), G (q1) and B (q4).
        "ee.zero.q         q2             \n"
        "ee.zero.q         q5             \n"
        "ee.zero.q         q6             \n"
        "ee.vzip.8         q2, q3         \n"
        "ee.vzip.8         q5, q1         \n"
        "ee.vzip.8         q6, q4         \n"
        "ee.vst.128.ip     q2, %[r], 0    \n"
        "ee.vst.128.ip     q5, %[g], 0    \n"
        "ee.vst.128.ip     q6, %[b], 0    \n"
        : [src] "+r"(src), [last] "+r"(last), [r] "+r"(r), [g] "+r"(g), [b] "+r"(b), [k] "+r"(k)
        :
        : "memory");
}

// The curve pass in place over pairs of vectors, pairs at least 1. Expected 46 cycles per 16
// values, 2.9 per value: 43 instructions and the taken branch. Each load and each multiply has one
// instruction between it and its first use. Each SAR write has one instruction between it and the
// multiply that reads it, and it is placed after the last multiply that reads the value before it.
void curve_pie(q15* a, int pairs)
{
    const int16_t* k   = k_curve;
    q15*           out = a;
    __asm__ volatile(
        "ssai              15             \n"
        "1:                               \n"
        "ee.vld.128.ip     q0, %[a], 16   \n" // t of vector A
        "ee.vld.128.ip     q1, %[a], 16   \n" // t of vector B
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // K
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // CLAMP
        "ee.vmul.s16       q2, q0, q6     \n" // lin = K t, q15
        "ee.vmul.s16       q3, q1, q6     \n"
        "ee.vmax.s16       q0, q0, q7     \n" // tc
        "ee.vmax.s16       q1, q1, q7     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // C5
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // C4
        "ee.vmul.s16       q4, q0, q6     \n" // C5 tc, q19
        "ee.vmul.s16       q5, q1, q6     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // C3
        "ee.vadds.s16      q4, q4, q7     \n" // stage 4, q19
        "ee.vadds.s16      q5, q5, q7     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // C2
        "ssai              19             \n"
        "ee.vadds.s16      q4, q4, q6     \n" // stage 3, q19
        "ee.vadds.s16      q5, q5, q6     \n"
        "ee.vmul.s16       q4, q4, q0     \n" // shift 19, q19 to q15
        "ee.vmul.s16       q5, q5, q1     \n"
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // C1
        "ssai              15             \n"
        "ee.vadds.s16      q4, q4, q7     \n" // stage 2, q15
        "ee.vadds.s16      q5, q5, q7     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // C0
        "addi              %[k], %[k], -16\n"
        "ee.vadds.s16      q4, q4, q6     \n" // stage 1, q15
        "ee.vadds.s16      q5, q5, q6     \n"
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vmul.s16       q5, q5, q1     \n"
        "addi              %[n], %[n], -1 \n"
        "ee.vadds.s16      q4, q4, q2     \n" // + lin
        "ee.vadds.s16      q5, q5, q3     \n"
        "ee.vadds.s16      q4, q4, q7     \n" // + C0, stage 0
        "ee.vadds.s16      q5, q5, q7     \n"
        "ee.vst.128.ip     q4, %[o], 16   \n"
        "ee.vst.128.ip     q5, %[o], 16   \n"
        "bnez              %[n], 1b       \n"
        : [a] "+r"(a), [o] "+r"(out), [k] "+r"(k), [n] "+r"(pairs)
        :
        : "memory");
}

// The curve pass in place over one vector, the steps of curve_pie on vector A. 26 instructions.
// Each load has one instruction between it and its first use, and so does each multiply but the
// last. The SAR writes keep the spacing of curve_pie.
void curve8_pie(q15* a)
{
    const int16_t* k = k_curve;
    __asm__ volatile(
        "ssai              15             \n"
        "ee.vld.128.ip     q0, %[a], 0    \n" // t
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // K
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // CLAMP
        "ee.vmul.s16       q2, q0, q6     \n" // lin = K t, q15
        "ee.vmax.s16       q0, q0, q7     \n" // tc
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // C5
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // C4
        "ee.vmul.s16       q4, q0, q6     \n" // C5 tc, q19
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // C3
        "ee.vadds.s16      q4, q4, q7     \n" // stage 4, q19
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // C2
        "ssai              19             \n"
        "ee.vadds.s16      q4, q4, q6     \n" // stage 3, q19
        "ee.vmul.s16       q4, q4, q0     \n" // shift 19, q19 to q15
        "ee.vldbc.16.ip    q6, %[k], 2    \n" // C1
        "ssai              15             \n"
        "ee.vadds.s16      q4, q4, q7     \n" // stage 2, q15
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vldbc.16.ip    q7, %[k], 2    \n" // C0
        "ee.vadds.s16      q4, q4, q6     \n" // stage 1, q15
        "ee.vmul.s16       q4, q4, q0     \n"
        "ee.vadds.s16      q4, q4, q2     \n" // + lin
        "ee.vadds.s16      q4, q4, q7     \n" // + C0, stage 0
        "ee.vst.128.ip     q4, %[a], 0    \n"
        : [a] "+r"(a), [k] "+r"(k)
        :
        : "memory");
}

// The curve pass over n values, n a multiple of 8.
void curve_all_pie(q15* a, int n)
{
    if(n >= 16) curve_pie(a, n / 16);
    if(n % 16) curve8_pie(a + n - 8);
}
} // namespace

void tone_fx_srgb_to_lin(const uint8_t* rgb, int n, q15* r, q15* g, q15* b)
{
    // The block loop reads up to the block that holds the byte after its last pixel, and the tail
    // up to the block that holds its byte 23 (tone_fx_srgb.h gives the bound).
    const int blocks = n / 16;
    if(blocks > 0) deinterleave_pie(rgb, blocks, r, g, b);
    if(n % 16)
    {
        const int done = 16 * blocks;
        deinterleave8_pie(rgb + 3 * done, r + done, g + done, b + done);
    }
    curve_all_pie(r, n);
    curve_all_pie(g, n);
    curve_all_pie(b, n);
}

#endif // TONE_FX_PIE
