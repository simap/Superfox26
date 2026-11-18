// The tone pass maps each near-neutral pixel to a pair of neutral tonemap entries that bracket its
// lightness.
// For each of the n lightness samples this module writes the darker entry index to lo and the
// lighter one to hi.
// Neutral entries are the tonemap entries whose pattern has no red.
// n must be a multiple of 8, and lo and hi must each have room for n bytes.
// lo and hi are equal for a sample whose lightness is within a small tolerance of a neutral entry,
// or lighter than the lightest neutral entry, or darker than the darkest.
#pragma once

#include "tone_fx.h"

#define TONE_FX_NEUTRAL_MAX 32

int            tone_fx_neutral_count();
const uint8_t* tone_fx_neutral_entries();

void tone_fx_bracket(const q15* L, int n, uint8_t* lo, uint8_t* hi);
