// Converts packed 8-bit sRGB pixels into three planar q15 arrays of linear light, one plane per
// channel, by decoding the sRGB transfer curve.
// rgb points at n pixels of interleaved R, G and B bytes, 3*n bytes in all, and r, g and b each
// receive n q15 values.
// n must be a multiple of 8.
// r, g and b must be 16-byte aligned.
#pragma once

#include "tone_fx.h"

void tone_fx_srgb_to_lin(const uint8_t* rgb, int n, q15* r, q15* g, q15* b);
