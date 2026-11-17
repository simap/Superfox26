// The constants of the Oklab kernels, shared by the vector forms in tone_fx_oklab.cpp and the stand
// in, host_tests/tone_fx/model/tone_fx_oklab_model.cpp. For those two and the tests only. The maths
// is described in tone_fx_oklab.cpp.
#pragma once

#include <cstdint>

namespace tone_fx_oklab_internal
{
// Cube root core, shared by both paths. F_r = 2^(-r/3) * 32767, so that F_0 fits q15: D is fitted
// 32768/32767 high to match. G_r = round(29559 * 2^(-r/3)).
constexpr int16_t CORE_F0 = 32767, CORE_F1 = 26007, CORE_F2 = 20642;
constexpr int16_t CORE_G0 = 29559, CORE_G1 = 23461, CORE_G2 = 18621;

// D per path: D = floor((KD + sum C_n * t^n) / 2^15), t^n in q15 by successive floor products,
// C_n in q15, KD the constant term at 2^15 plus the rounding half, a product of two constants as the
// vector unit forms it. The exact path (q15 cube root) fits p at x'', the floor path (Oklab) at
// x'' + 1/2. KO is the offset and rounding of the last read out, at shift 14 for q15 out and 13 for
// offset q16 out.
struct core_path
{
    int16_t c1, c2, c3, c4, c5;
    int16_t kd_a, kd_b;
    int16_t ko_a, ko_b;
    int     out_shift;
};
constexpr core_path PATH_EXACT = {26465, -2937, 543, -132, 33, 19477, 2861, 8192, 1, 14};
constexpr core_path PATH_FLOOR = {26465, -2937, 543, -132, 33, 26962, 2069, 32640, -8224, 13};

// Linear sRGB to LMS, q15, each row rounded to sum to 32768 so that white maps to LMS 1 1 1.
constexpr int16_t M1[3][3] = {{13508, 17574, 1686}, {6944, 22305, 3519}, {2894, 9231, 20643}};

// LMS cube roots to Oklab, q15, rows rounded so that L of white is 32768 and a and b of any gray are
// 0. A coefficient above 1 in magnitude is split into parts that each fit q15: a takes 1.978 as
// 32407 + 32408 and -2.429 as -26527 - 26527 - 26526.
constexpr int16_t M2_L[3]   = {6896, 26005, -133};
constexpr int16_t M2_A_L[2] = {32407, 32408};
constexpr int16_t M2_A_M[3] = {-26527, -26527, -26526};
constexpr int16_t M2_A_S    = 14765;
constexpr int16_t M2_B[3]   = {849, 25650, -26499};

// Oklab to the LMS cube roots, q15. L enters as 2 * 16384. s takes -1.2915 as two parts.
constexpr int16_t M3_LA = 12987, M3_LB = 7071;
constexpr int16_t M3_MA = -3459, M3_MB = -2092;
constexpr int16_t M3_SA = -2932, M3_SB0 = -21160, M3_SB1 = -21159;
// Cubes to linear sRGB, q15, split into parts that fit q15. Each row sums to 32768.
constexpr int16_t M4_RL0 = 26718, M4_RL1 = 26717;   // 4.0767 = 2 * RL0 + 3 * RL1
constexpr int16_t M4_RM0 = -27097, M4_RM1 = -27096; // -3.3077 = 3 * RM0 + RM1
constexpr int16_t M4_RS  = 7568;                    // 0.2310
constexpr int16_t M4_GL  = -20782;                  // -1.2684 = 2 * GL
constexpr int16_t M4_GM0 = 28505, M4_GM1 = 28506;   // 2.6098 = 2 * GM0 + GM1
constexpr int16_t M4_GS  = -11184;                  // -0.3413
constexpr int16_t M4_BL  = -137;                    // -0.0042
constexpr int16_t M4_BM  = -23050;                  // -0.7034
constexpr int16_t M4_BS0 = 27977, M4_BS1 = 27978;   // 1.7076 = BS0 + BS1
} // namespace tone_fx_oklab_internal
