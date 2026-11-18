// Limits the colour of each pixel to the most its lightness can carry.
// For n samples it fits the A and B chroma against a ceiling derived from L, rewrites the L, A and
// B channels into Lo, Ao and Bo, and returns in Co the chroma that survived, which the caller tests
// to treat the pixel as neutral or coloured.
// The kernel reads and writes samples eight at a time, so every buffer must hold n rounded up to a
// multiple of eight (tone_fx_pad8) and must be 16-byte aligned.
// Lo, Ao and Bo may point at L, A and B to run in place; Co needs its own buffer.
#pragma once

#include "tone_fx.h"

void tone_fx_squeeze(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co);
