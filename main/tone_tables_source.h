// Fingerprints the inputs to the tone table generator so a stale main/tone_tables.inc is caught at
// build time.
// The generated file stores the value as source_hash and tone_tables.h asserts it still matches
// tone_tables_source_hash().
// The fingerprint covers k_tonemap and a fixed list of tonemap, phase-shape, sequence and
// push-limit constants, not every input the generator reads, so a change to a constant outside that
// list does not trip the assert.
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
    x.f32(TONEMAP_CHROMA_WEIGHT);
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

struct tone_white_slot
{
    q15     r, g, b, L, A, B, C;
    uint8_t lo, hi, kind, ent, white_kind;
};
