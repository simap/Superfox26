#include "tone_fx_bracket.h"

#include <cstring>

#include "tone_fx_bracket_internal.h"
#include "tone_tables.h"

namespace
{
const int s_n_steps = tone_tables::bracket_steps;

static_assert(tone_tables::neutral_count >= 1 && tone_tables::neutral_count <= TONE_FX_NEUTRAL_MAX,
              "the neutral list fits the kernel");
static_assert(s_n_steps % 8 == 0, "the kernel runs the steps 8 at a time");

TONE_TABLES_RAM alignas(16) std::array<int16_t, 2 + 2 * tone_tables::bracket_steps> s_table = tone_tables::bracket;

#if TONE_FX_PIE
#define BRACKET_STEP                                                                                                   \
    "ee.vldbc.16.ip    q4, %[tp], 2    \n"                                                                             \
    "ee.vldbc.16.ip    q5, %[tp], 2    \n"                                                                             \
    "ee.vcmp.lt.s16    q6, q0, q4      \n"                                                                             \
    "ee.vcmp.lt.s16    q7, q1, q4      \n"                                                                             \
    "ee.andq           q6, q6, q5      \n"                                                                             \
    "ee.andq           q7, q7, q5      \n"                                                                             \
    "ee.vadds.s16      q2, q2, q6      \n"                                                                             \
    "ee.vadds.s16      q3, q3, q7      \n"

void bracket_pie(const q15* L, int pairs, uint8_t* lo, uint8_t* hi)
{
    const int16_t* base   = s_table.data();
    const int      groups = s_n_steps / 8;
    const int16_t* tp;
    int            g;
    __asm__ volatile(
        "1:                                \n"
        "ee.vld.128.ip     q0, %[L], 16    \n"
        "mov               %[tp], %[base]  \n"
        "ee.vld.128.ip     q1, %[L], 16    \n"
        "mov               %[g], %[groups] \n"
        "ee.vldbc.16.ip    q2, %[tp], 4    \n"
        "addi              %[n], %[n], -1  \n"
        "ee.orq            q3, q2, q2      \n"
        "2:                                \n"
        BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP BRACKET_STEP
        "ee.vldbc.16.ip    q4, %[tp], 2    \n"
        "ee.vldbc.16.ip    q5, %[tp], 2    \n"
        "ee.vcmp.lt.s16    q6, q0, q4      \n"
        "ee.vcmp.lt.s16    q7, q1, q4      \n"
        "addi              %[g], %[g], -1  \n"
        "ee.andq           q6, q6, q5      \n"
        "ee.andq           q7, q7, q5      \n"
        "ee.vadds.s16      q2, q2, q6      \n"
        "ee.vadds.s16      q3, q3, q7      \n"
        "bnez              %[g], 2b        \n"
        "ee.vunzip.8       q2, q3          \n"
        "ee.vst.128.ip     q2, %[lo], 16   \n"
        "ee.vst.128.ip     q3, %[hi], 16   \n"
        "bnez              %[n], 1b        \n"
        : [L] "+r"(L), [lo] "+r"(lo), [hi] "+r"(hi), [n] "+r"(pairs), [tp] "=&r"(tp), [g] "=&r"(g)
        : [base] "r"(base), [groups] "r"(groups)
        : "memory");
}
#undef BRACKET_STEP
#endif
}

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
