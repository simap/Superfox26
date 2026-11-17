// The squeeze of tonemap_squeeze.h in fixed point. It brings an image colour in Oklab into the
// range the panel shows. The float code is the reference. The steps and their fixed point form are
// in tone_fx_squeeze.cpp.
//
// Per colour, all q15 (tone_fx.h):
//   in   L, a, b of the image colour
//   out  Lo  the squeezed L, held at or above the L of the black entry
//        Ao, Bo  the squeezed a and b, on the hue of sRGB red
//        Co  the squeezed chroma along that hue, 0 or above. It is exactly 0 for a colour whose
//            chroma along the hue is 0 or below (a gray, a blue, a green).
//
// Arrays are planar, 16 byte aligned and n is a multiple of 8 (tone_fx.h). Lo, Ao, Bo and Co may
// be the input arrays themselves, L as Lo, a as Ao and b as Bo.
#pragma once

#include "tone_fx.h"

// The squeeze of n colours. Reads the parameters of main/tone_tables.inc, fitted on the host to
// the tone table and sRGB (host_tests/tone_tables).
void tone_fx_squeeze(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co);
