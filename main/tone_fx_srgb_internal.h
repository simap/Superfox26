// Coefficients of the fixed-point sRGB to linear transfer curve that the kernels in
// tone_fx_srgb.cpp evaluate.
// K scales each sample, CLAMP is the lower bound it is held to, and C5 through C0 are the
// polynomial coefficients applied in that order.
#pragma once

#include <cstdint>

namespace tone_fx_srgb_internal
{
const int16_t K     = 1280;
const int16_t CLAMP = -29952;
const int16_t C5    = 989;
const int16_t C4    = -3022;
const int16_t C3    = 18433;
const int16_t C2    = 9665;
const int16_t C1    = 14021;
const int16_t C0    = 7073;
}
