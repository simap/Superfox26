// Shared definitions for the tone_fx color kernels: the q15 fixed-point type, its conversions, and
// the switch that selects the vector build.
// TONE_FX_PIE is 1 on an ESP build and 0 on any other.
// An ESP build whose target has no vector unit fails to compile.
// Where TONE_FX_PIE is 0 the kernel bodies are left out with no fallback, so the kernels build only
// for the ESP32-S3 and host tests use separate models.
// The kernels work eight elements at a time, so a caller rounds element counts up to a multiple of
// eight with tone_fx_pad8.
#pragma once

#include <cstdint>

#if defined(ESP_PLATFORM)
#include "soc/soc_caps.h"
#if !SOC_SIMD_INSTRUCTION_SUPPORTED
#error "the tone_fx kernels need the vector unit of the ESP32-S3"
#endif
#define TONE_FX_PIE 1
#else
#define TONE_FX_PIE 0
#endif

typedef int16_t q15;

static const q15 Q15_ONE = 32767;

#define TONE_FX_ALIGNED alignas(16)

static inline int tone_fx_pad8(int n)
{
    return (n + 7) & ~7;
}

static inline q15 q15_from(float v)
{
    const float s = v * 32768.0f + (v < 0.0f ? -0.5f : 0.5f);
    return s >= 32767.0f ? Q15_ONE : s <= -32768.0f ? static_cast<q15>(-32768) : static_cast<q15>(s);
}

static inline float q15_to(q15 v)
{
    return static_cast<float>(v) * (1.0f / 32768.0f);
}
