// What main/tone_tables.inc is built from, for its generator (host_tests/tone_tables) and for the
// firmware build that checks it (tone_tables.h).
//
// tone_tables_source_hash() is a 64 bit FNV-1a hash of the tone table and of every constant of
// main/ and components/epd97 that the generated tables depend on: each entry's L, a and b as float
// bits and its pattern, the paper and black patterns, TONEMAP_VDHR_PCT, TONEMAP_COMPRESS,
// TONEMAP_GAMMA, TONEMAP_CIE_CHROMA_WEIGHT, the four phase 1 shapes, EPD_PHASE1_DRIVE,
// EPD_SEQ_W_FRAMES, TONE_NUDGE_FRAMES, TONE_SEQ_MAX and the three push limits. The generator writes
// the hash it saw into the file, and tone_tables.h compares it with the hash of the headers the
// firmware compiles. The kernel constants of main/tone_fx_*_internal.h and the code of the
// generator also shape the tables. A change there is caught by make test in host_tests/tone_tables,
// which regenerates the file and compares.
#pragma once

#include <cstdint>

#include "epd97_config.h"
#include "epd97_lut.h"
#include "tone_fx.h"
#include "tone_plan.h"
#include "tonemap.h"

struct tone_tables_hash
{
    uint64_t h = 14695981039346656037ull;

    constexpr void byte(uint8_t b)
    {
        h ^= b;
        h *= 1099511628211ull;
    }
    constexpr void u32(uint32_t v)
    {
        for(int i = 0; i < 4; i++) byte(static_cast<uint8_t>(v >> (8 * i)));
    }
    constexpr void f32(float v)
    {
        u32(__builtin_bit_cast(uint32_t, v));
    }
    // The characters and the terminating NUL.
    constexpr void str(const char* s)
    {
        for(; *s != '\0'; s++) byte(static_cast<uint8_t>(*s));
        byte(0);
    }
};

constexpr uint64_t tone_tables_source_hash()
{
    tone_tables_hash x;
    x.u32(static_cast<uint32_t>(TONEMAP_COUNT));
    for(const tone_entry& t : k_tonemap)
    {
        x.f32(t.L);
        x.f32(t.a);
        x.f32(t.b);
        x.str(t.pattern);
    }
    x.str(TONEMAP_PAPER_PATTERN);
    x.str(TONEMAP_BLACK_PATTERN);
    x.u32(TONEMAP_VDHR_PCT);
    x.f32(TONEMAP_COMPRESS);
    x.f32(TONEMAP_GAMMA);
    x.f32(TONEMAP_CIE_CHROMA_WEIGHT);
    x.str(EPD_PHASE1_SHAPE_S);
    x.str(EPD_PHASE1_SHAPE_I);
    x.str(EPD_PHASE1_SHAPE_P);
    x.str(EPD_PHASE1_SHAPE_M);
    x.u32(EPD_PHASE1_DRIVE);
    x.u32(EPD_SEQ_W_FRAMES);
    x.u32(TONE_NUDGE_FRAMES);
    x.u32(TONE_SEQ_MAX);
    x.u32(EPD_PUSH_MAX_VDH);
    x.u32(EPD_PUSH_MAX_VDL);
    x.u32(EPD_PUSH_MAX_VDHR);
    return x.h;
}

// The slot WHITE of the dither's chunk (tones.cpp) as pass A leaves it for a white pixel: the
// target in linear light (r, g, b), the squeezed colour (L, A, B, C), the neutral pair, the slot
// kind and entry, and white_kind, the kind the dither takes when white is not on the paper entry.
struct tone_white_slot
{
    q15     r, g, b, L, A, B, C;
    uint8_t lo, hi, kind, ent, white_kind;
};
