// The two neutral entries around a lightness. See tone_fx_bracket.h.
//
// As L rises from 0 to 32767, lo and hi each step through the neutral entries from the darkest to
// the lightest, so the pair is a step function of L with at most 2 (n - 1) steps for n neutral
// entries. The generator of main/tone_tables.inc (host_tests/tone_tables) finds every L where the
// pair changes by running the comparisons of bracket() in float on each q15 value, so the steps
// fall exactly where the float code puts them. The pair is packed in one int16, S = lo + 256 hi,
// which needs entry indices below 128.
//
// For a value L the kernel starts from S at L 32767 and, for every step with threshold T and delta
// P = S(T - 1) - S(T), adds P when L < T. The steps run from the highest T down, so after each add
// the sum is S at some L, between 0 and 32767, and the saturating add never saturates. The stand in
// of the host tests does the same in the same order.
//
// Kernel, 16 values (2 vectors) per loop. Per step: two broadcast loads (T and P), two
// ee.vcmp.lt.s16, two ee.andq and two ee.vadds.s16, 8 ops for 16 values. The steps run 8 per inner
// loop, the table padded with steps that never fire (T -32768, P 0). After the steps ee.vunzip.8
// of the two sums puts the 16 low bytes, lo, in one vector and the 16 high bytes, hi, in the other.

#include "tone_fx_bracket.h"

#include <cstring>

#include "tone_fx_bracket_internal.h"
#include "tone_tables.h"

namespace
{
const int s_n_steps = tone_tables::bracket_steps; // padded to a multiple of 8

static_assert(tone_tables::neutral_count >= 1 && tone_tables::neutral_count <= TONE_FX_NEUTRAL_MAX,
              "the neutral list fits the kernel");
static_assert(s_n_steps % 8 == 0, "the kernel runs the steps 8 at a time");

// [0] the state at L 32767, [1] 0, then T and P per step, T falling. The kernel loads [0] and steps
// its pointer by 4 bytes to the first T. Read per chunk: internal RAM, initialized data.
TONE_TABLES_RAM alignas(16) std::array<int16_t, 2 + 2 * tone_tables::bracket_steps> s_table = tone_tables::bracket;

#if TONE_FX_PIE
// One step on vectors A (L in q0, sum in q2) and B (q1, q3): T to q4 and P to q5, then where
// L < T the compare gives all ones and the sum takes P. T is loaded two instructions before the
// compares read it, P three before the ANDs.
#define BRACKET_STEP                                                                                                   \
    "ee.vldbc.16.ip    q4, %[tp], 2    \n"                                                                             \
    "ee.vldbc.16.ip    q5, %[tp], 2    \n"                                                                             \
    "ee.vcmp.lt.s16    q6, q0, q4      \n"                                                                             \
    "ee.vcmp.lt.s16    q7, q1, q4      \n"                                                                             \
    "ee.andq           q6, q6, q5      \n"                                                                             \
    "ee.andq           q7, q7, q5      \n"                                                                             \
    "ee.vadds.s16      q2, q2, q6      \n"                                                                             \
    "ee.vadds.s16      q3, q3, q7      \n"

// pairs of vectors, at least 1. Expected per 16 values, with S steps: 8 cycles per step plus about
// 5 per group of 8 for the loop count and the taken branch, and about 14 per pair outside the step
// loop. For the 40 steps of the current table about 359 cycles, 22.4 per value. No load is used by
// the next instruction and the compares, ANDs and adds forward to each other. The number rests
// most on the taken branch costing about 3 cycles.
void bracket_pie(const q15* L, int pairs, uint8_t* lo, uint8_t* hi)
{
    const int16_t* base   = s_table.data();
    const int      groups = s_n_steps / 8;
    const int16_t* tp;
    int            g;
    __asm__ volatile(
        "1:                                \n"
        "ee.vld.128.ip     q0, %[L], 16    \n" // L of vector A
        "mov               %[tp], %[base]  \n"
        "ee.vld.128.ip     q1, %[L], 16    \n" // L of vector B
        "mov               %[g], %[groups] \n"
        "ee.vldbc.16.ip    q2, %[tp], 4    \n" // sum A = S at L 32767, tp to the first T
        "addi              %[n], %[n], -1  \n"
        "ee.orq            q3, q2, q2      \n" // sum B
        "2:                                \n" //
        BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP
        "ee.vldbc.16.ip    q4, %[tp], 2    \n" // the eighth step, with the loop count between
        "ee.vldbc.16.ip    q5, %[tp], 2    \n"
        "ee.vcmp.lt.s16    q6, q0, q4      \n"
        "ee.vcmp.lt.s16    q7, q1, q4      \n"
        "addi              %[g], %[g], -1  \n"
        "ee.andq           q6, q6, q5      \n"
        "ee.andq           q7, q7, q5      \n"
        "ee.vadds.s16      q2, q2, q6      \n"
        "ee.vadds.s16      q3, q3, q7      \n"
        "bnez              %[g], 2b        \n"
        "ee.vunzip.8       q2, q3          \n" // q2 = low bytes (lo), q3 = high bytes (hi)
        "ee.vst.128.ip     q2, %[lo], 16   \n"
        "ee.vst.128.ip     q3, %[hi], 16   \n"
        "bnez              %[n], 1b        \n"
        : [L] "+r"(L), [lo] "+r"(lo), [hi] "+r"(hi), [n] "+r"(pairs), [tp] "=&r"(tp), [g] "=&r"(g)
        : [base] "r"(base), [groups] "r"(groups)
        : "memory");
}
#undef BRACKET_STEP
#endif
} // namespace

int tone_fx_neutral_count()
{
    return tone_tables::neutral_count;
}

const uint8_t* tone_fx_neutral_entries()
{
    return tone_tables::neutral.data();
}

int tone_fx_bracket_steps()
{
    return s_n_steps;
}

const int16_t* tone_fx_bracket_table()
{
    return s_table.data();
}

#if TONE_FX_PIE
void tone_fx_bracket(const q15* L, int n, uint8_t* lo, uint8_t* hi)
{
    if(n >= 16) bracket_pie(L, n / 16, lo, hi);
    if(n % 16)
    {
        // The last 8 values through 16 element buffers.
        alignas(16) q15     tl[16];
        alignas(16) uint8_t tlo[16], thi[16];
        const int           at = n - 8;
        memcpy(tl, L + at, 8 * sizeof(q15));
        memcpy(tl + 8, L + at, 8 * sizeof(q15));
        bracket_pie(tl, 1, tlo, thi);
        memcpy(lo + at, tlo, 8);
        memcpy(hi + at, thi, 8);
    }
}
#endif
