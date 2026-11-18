// Converts colour between linear RGB and OKLab, and takes the cube root, on q15 fixed-point arrays
// for the tone pipeline.
// The kernels are hand-written ESP32-S3 vector assembly, so the implementation is compiled only
// where TONE_FX_PIE is set.
// Callers must pass n as a multiple of 8: each kernel processes eight lanes at a time and handles
// no remainder.
// The input and output arrays must be 16-byte aligned.
#pragma once

#include "tone_fx.h"

void tone_fx_cbrt(const q15* x, int n, q15* y);

void tone_fx_lin_to_oklab(const q15* r, const q15* g, const q15* b, int n, q15* L, q15* A, q15* B);

void tone_fx_oklab_to_lin(const q15* L, const q15* A, const q15* B, int n, q15* r, q15* g, q15* b);

void tone_fx_lin_to_oklab_1(const q15 rgb[8], q15 lab[8]);
