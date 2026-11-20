// The stand ins of the tone_fx kernels (main/tone_fx.h), for host builds and the kernel probes, and
// what the tests read beside them.
//
// Each tone_fx_<unit>_model.cpp here defines the kernels of main/tone_fx_<unit>.h under their plain
// names as scalar C++ that models the vector instructions lane by lane. A host build compiles it in
// place of the vector forms. A probe compiles both, so its build renames each stand in with the
// suffix _model (probes/*/main/CMakeLists.txt), and the self test calls the names below.
#pragma once

#include <cstdint>

#include "tone_fx.h"

// The decode of main/tone_fx_srgb.h for one byte, as the stand in computes it.
q15 tone_fx_srgb_byte(uint8_t v);

// The stand ins under the names a probe build gives them.
void tone_fx_srgb_to_lin_model(const uint8_t* rgb, int n, q15* r, q15* g, q15* b);
void tone_fx_bracket_model(const q15* L, int n, uint8_t* lo, uint8_t* hi);
void tone_fx_squeeze_model(const q15* L, const q15* A, const q15* B, int n, q15* Lo, q15* Ao, q15* Bo, q15* Co);
void tone_fx_search_model(q15 L, q15 A, q15 B, uint8_t* best, uint8_t* next);
void tone_fx_cbrt_model(const q15* x, int n, q15* y);
void tone_fx_lin_to_oklab_model(const q15* r, const q15* g, const q15* b, int n, q15* L, q15* A, q15* B);
void tone_fx_oklab_to_lin_model(const q15* L, const q15* A, const q15* B, int n, q15* r, q15* g, q15* b);
void tone_fx_lin_to_oklab_1_model(const q15 rgb[8], q15 lab[8]);
