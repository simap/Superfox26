// The curve constants of tone_fx_srgb_to_lin(), shared by the vector form in tone_fx_srgb.cpp and
// the stand in, host_tests/tone_fx/model/tone_fx_srgb_model.cpp. For those two and the tests only.
// The curve and its arithmetic are described in tone_fx_srgb.cpp.
#pragma once

#include <cstdint>

namespace tone_fx_srgb_internal
{
// From host_tests/tone_fx/srgb/fit.cpp.
// K: the linear segment is 32768 / (255 * 12.92) = 9.946 counts per byte. K is 10 counts per byte,
// 10 * 128 per unit of t, so K t has no rounding, and the 110 counts from byte 11 down to byte 0
// put byte 0 at exactly 0. Byte 10 then gives 100 against 99.
const int16_t K     = 1280;
const int16_t CLAMP = -29952; // t of byte 11, (11 - 128) * 256
const int16_t C5    = 989;    // q19
const int16_t C4    = -3022;  // q19
const int16_t C3    = 18433;  // q19
const int16_t C2    = 9665;   // q15
const int16_t C1    = 14021;  // q15
const int16_t C0    = 7073;   // q15
} // namespace tone_fx_srgb_internal
