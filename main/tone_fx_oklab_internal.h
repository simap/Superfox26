// Fixed-point int16_t coefficients for the OKLab conversions in tone_fx_oklab.cpp.
// CORE_* and PATH_* feed the cube root path, M1 and M2_* feed linear RGB to OKLab, and M3_* and
// M4_* feed OKLab to linear RGB.
// The constants sit in this header, apart from the public declarations in tone_fx_oklab.h, so that
// the source and the host test model share one copy.
// The host model reads the constants directly in C++.
#pragma once

#include <cstdint>

namespace tone_fx_oklab_internal
{
constexpr int16_t CORE_F0 = 32767, CORE_F1 = 26007, CORE_F2 = 20642;
constexpr int16_t CORE_G0 = 29559, CORE_G1 = 23461, CORE_G2 = 18621;

struct core_path
{
    int16_t c1, c2, c3, c4, c5;
    int16_t kd_a, kd_b;
    int16_t ko_a, ko_b;
    int     out_shift;
};
constexpr core_path PATH_EXACT = {26465, -2937, 543, -132, 33, 19477, 2861, 8192, 1, 14};
constexpr core_path PATH_FLOOR = {26465, -2937, 543, -132, 33, 26962, 2069, 32640, -8224, 13};

constexpr int16_t M1[3][3] = {{13508, 17574, 1686}, {6944, 22305, 3519}, {2894, 9231, 20643}};

constexpr int16_t M2_L[3]   = {6896, 26005, -133};
constexpr int16_t M2_A_L[2] = {32407, 32408};
constexpr int16_t M2_A_M[3] = {-26527, -26527, -26526};
constexpr int16_t M2_A_S    = 14765;
constexpr int16_t M2_B[3]   = {849, 25650, -26499};

constexpr int16_t M3_LA = 12987, M3_LB = 7071;
constexpr int16_t M3_MA = -3459, M3_MB = -2092;
constexpr int16_t M3_SA = -2932, M3_SB0 = -21160, M3_SB1 = -21159;
constexpr int16_t M4_RL0 = 26718, M4_RL1 = 26717;
constexpr int16_t M4_RM0 = -27097, M4_RM1 = -27096;
constexpr int16_t M4_RS  = 7568;
constexpr int16_t M4_GL  = -20782;
constexpr int16_t M4_GM0 = 28505, M4_GM1 = 28506;
constexpr int16_t M4_GS  = -11184;
constexpr int16_t M4_BL  = -137;
constexpr int16_t M4_BM  = -23050;
constexpr int16_t M4_BS0 = 27977, M4_BS1 = 27978;
}
