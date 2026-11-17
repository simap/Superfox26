// sRGB bytes to linear light in q15. The format and the array rules are in tone_fx.h.
//
// The decode of one byte v, with c = v / 255: c / 12.92 at or below 0.04045 (bytes 0 to 10), else
// ((c + 0.055) / 1.055) ^ 2.4. The kernel computes it as a degree 5 polynomial in 16 bit fixed point
// (tone_fx_srgb.cpp). It is within 1 count of the decode rounded to nearest at every byte, exact at
// bytes 0 to 9, 11 and 255, and never decreases as the byte rises (host_tests/tone_fx/srgb). Byte
// 255 gives Q15_ONE.
#pragma once

#include "tone_fx.h"

// rgb holds n pixels as interleaved R, G, B bytes, 3 n bytes at any alignment. n is a multiple of 8.
// r, g and b each receive n q15 values, planar, 16 byte aligned.
//
// Reads. The kernel loads whole aligned 16 byte blocks, from the block that holds rgb[0] up to the
// block that holds rgb[3 n + 15] at most. No block it loads holds a byte at or past rgb + 3 n + 16.
// So it reads up to 16 bytes past the last pixel, and the caller makes those 16 bytes readable. The
// bytes it reads before rgb lie in the block of rgb[0]. It uses no byte outside rgb[0] to
// rgb[3 n - 1].
//
// Writes. r[0] to r[n - 1], g[0] to g[n - 1] and b[0] to b[n - 1], and nothing else.
void tone_fx_srgb_to_lin(const uint8_t* rgb, int n, q15* r, q15* g, q15* b);
