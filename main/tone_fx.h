// Fixed point for the tone code, shared by the tone_fx_* kernels.
//
// Format. A q15 is a signed 16 bit value, the real number times 32768. It holds -1 to just under 1.
// The real 1.0 is stored as Q15_ONE, 32767. Oklab L, a and b, linear light and carried error are
// all q15. A kernel may use another scale inside, for a coefficient above 1 for example, and says so
// where it does.
//
// Arrays. A kernel that takes arrays takes them planar, one array per component, each aligned to 16
// bytes and a multiple of 8 elements long. The vector loads and stores ignore the low 4 address bits,
// so an unaligned array reads and writes the wrong bytes without a fault.
//
// Device and host. Each kernel in main/ is written for the ESP32-S3 vector unit and builds only in
// an ESP-IDF build, where TONE_FX_PIE is 1. The tables the kernels read are data, built on the host
// into main/tone_tables.inc (host_tests/tone_tables), and build everywhere. Host builds link a stand
// in for each kernel under the same name, from host_tests/tone_fx/model: scalar C++ that models the
// vector instructions lane by lane and gives the same bits for every input. The kernel probes in probes/ check the two against each other under
// QEMU. What a stand in shares with main/ is in main/tone_fx_*_internal.h.
//
// Vector unit. 8 lanes of 16 bit or 16 lanes of 8 bit in 128 bit registers. It has no divide, no
// float and no load by per lane index, so a kernel calculates in lanes where scalar code would read
// a table by index, and it divides by a multiply with a stored reciprocal or by a shift.
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

// A count rounded up to the 8 lanes of a 16 bit vector.
static inline int tone_fx_pad8(int n)
{
    return (n + 7) & ~7;
}

// A real number to q15, rounded to nearest and held to the q15 range. For tables and tests, not for
// per pixel code.
static inline q15 q15_from(float v)
{
    const float s = v * 32768.0f + (v < 0.0f ? -0.5f : 0.5f);
    return s >= 32767.0f ? Q15_ONE : s <= -32768.0f ? static_cast<q15>(-32768) : static_cast<q15>(s);
}

static inline float q15_to(q15 v)
{
    return static_cast<float>(v) * (1.0f / 32768.0f);
}
