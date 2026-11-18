// The panel can reach only a fixed set of tones.
// For each tone this table records its colour as L, a, b and the drive pattern string that makes
// the panel show it, so the renderer can match a wanted colour to a tone and produce it.
// The file also sets the constants that tune how an input colour is pulled onto this set.
#pragma once

#include <stdint.h>

// 44 is the red drive fraction at which the panel's stock red waveform, of 220 VDH, 310 VDL and 204
// VDHR frames, is DC balanced.
#define TONEMAP_VDHR_PCT 44

#define TONEMAP_PAPER_PATTERN "SM3p15"
#define TONEMAP_BLACK_PATTERN "SM3i15"

#define TONEMAP_COMPRESS 0.75f

#define TONEMAP_GAMMA 1.0f

#define TONEMAP_CHROMA_WEIGHT 2.0f

struct tone_entry
{
    float       L, a, b;
    const char* pattern;
};

static constexpr tone_entry k_tonemap[] = {
#include "tonemap_entries.inc"
};

#define TONEMAP_COUNT (sizeof(k_tonemap) / sizeof(k_tonemap[0]))

static_assert(TONEMAP_COUNT <= 256, "a frame holds the entry index in one byte");
