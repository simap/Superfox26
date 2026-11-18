// Finds the two entries of the tone map k_tonemap closest in color to a point: the nearest and the
// next nearest.
#pragma once

#include <cstdint>

#include "tone_fx.h"

void tone_fx_search(q15 L, q15 A, q15 B, uint8_t* best, uint8_t* next);
