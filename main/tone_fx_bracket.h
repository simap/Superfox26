// The two neutral entries around a lightness, in q15. The format and the array rules are in
// tone_fx.h.
//
// A neutral entry is an entry of k_tonemap (tonemap.h) whose pattern holds no 'r' and no 'R'. The
// kernel gives for each L what bracket() of tones_float_ref.inc gives for the float L / 32768: lo
// the darker and hi the lighter of the two neutral entries around L, both the same entry when L is
// within ON_ENTRY_L (0.002) of an entry, above the lightest or below the darkest. It holds for every
// q15 value, 0 to Q15_ONE and the negative ones (host_tests/tone_fx/bracket).
#pragma once

#include "tone_fx.h"

// The most neutral entries the kernel is built for.
#define TONE_FX_NEUTRAL_MAX 32

// The neutral entries, lightest first, as entry indices into k_tonemap. From main/tone_tables.inc,
// built on the host. At most TONE_FX_NEUTRAL_MAX, each index below 128.
int            tone_fx_neutral_count();
const uint8_t* tone_fx_neutral_entries();

// L: n q15 lightness values, 16 byte aligned, n a multiple of 8. lo and hi: n bytes each, 16 byte
// aligned, the entry indices into k_tonemap. Reads the step table of main/tone_tables.inc and no
// float.
void tone_fx_bracket(const q15* L, int n, uint8_t* lo, uint8_t* hi);
