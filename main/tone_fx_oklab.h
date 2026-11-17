// Oklab kernels in fixed point: the cube root, linear sRGB to Oklab and Oklab to linear sRGB. The
// format and the array rules are those of tone_fx.h. The maths is that of lin_to_oklab() and
// oklab_to_lin() in tones.cpp.
//
// Largest error in counts of q15, from host_tests/tone_fx/oklab (make test prints the distribution
// and the worst inputs):
//   tone_fx_cbrt   0.67 against cbrt over all 32767 positive inputs.
//   lin to Oklab   L 1.22, a 3.40, b 1.40 against lin_to_oklab() over every sRGB byte colour, 4M
//                  random q15 colours and every colour within 63 counts of black.
//   Oklab to lin   r 3.58, g 2.05, b 1.55 against oklab_to_lin() over the q15 Oklab of every sRGB
//                  byte colour and of 4M random q15 colours.
#pragma once

#include "tone_fx.h"

// y = cube root of x. x in 0 to Q15_ONE, a negative x counts as 0. n is a multiple of 8, x and y
// 16 byte aligned. y may be x.
void tone_fx_cbrt(const q15* x, int n, q15* y);

// Linear sRGB in 0 to Q15_ONE to Oklab L, a and b. Planar arrays as in tone_fx.h. The outputs may be
// the inputs. Out of range, an LMS value at or below 0 gives a cube root of 0.
void tone_fx_lin_to_oklab(const q15* r, const q15* g, const q15* b, int n, q15* L, q15* A, q15* B);

// Oklab to linear sRGB, each result held to 0 to Q15_ONE. For an in-gamut input no intermediate
// wraps or saturates. Out of gamut, the cube roots l, m and s of LMS are held to 0 to 1 before they
// are cubed, and the results are held to 0 to Q15_ONE. Planar arrays as in tone_fx.h. The outputs
// may be the inputs.
void tone_fx_oklab_to_lin(const q15* L, const q15* A, const q15* B, int n, q15* r, q15* g, q15* b);

// One pixel: rgb holds r, g, b in elements 0 to 2, lab receives L, a, b in elements 0 to 2. The
// other elements of rgb are not read. The kernel writes all 8 elements of lab, 3 to 7 with values of
// no meaning, so lab is 16 byte aligned. The result has the same bits as tone_fx_lin_to_oklab for the
// same pixel.
void tone_fx_lin_to_oklab_1(const q15 rgb[8], q15 lab[8]);
