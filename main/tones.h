// Tone rendering: RGB pixels to tone map entries by error diffusion, and the frame played onto the
// panel as the two phases of tone_plan.h. The tone map is tonemap.h: per entry its Oklab L, a, b
// and the pattern of pulls that makes it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

static const int TONE_W = 960;
static const int TONE_H = 672;

// Entry index of the paper tone. The frame buffer is cleared to it.
static const uint8_t TONE_PAPER = 0;

// The float dither of the reference (tone_dither_float) is in the build of the host harness
// (-DTONES_FLOAT_REF) and of the benchmark.
#if defined(TONES_BENCH) || defined(TONES_FLOAT_REF)
#define TONES_HAVE_FLOAT_REF 1
#else
#define TONES_HAVE_FLOAT_REF 0
#endif

// Check the expanded patterns of main/tone_tables.inc against the push limits and the balance rule
// (tone_plan_init), place the dither's slot WHITE, and allocate and clear the frame. Every table is
// data built on the host (tone_tables.h). Call once after epd_begin(). False, with the reason
// logged, when a pattern fails the check or the frame does not fit in PSRAM. Then nothing draws.
bool tones_init();

// Entry index of the panel black, TONEMAP_BLACK_PATTERN.
uint8_t tones_black();

// The gray byte (r = g = b) nearest to g whose target lands exactly on a neutral entry, so an area
// filled with it renders solid, without dither. For UI fills such as the tab bar. A table lookup.
int tones_solid_gray(int g);

// The frame: TONE_W x TONE_H entry indices, row major.
uint8_t* tones_frame();

// Error diffusion dither of RGB rows into entry indices, serpentine, in fixed point (q15, tone_fx.h).
// A pixel's target is its sRGB colour in Oklab brought into the panel's range by the squeeze
// (tone_fx_squeeze.h), with L held at or above the black entry's. A target with no chroma after the
// squeeze is neutral: it mixes only the two neutral entries (patterns with no r or R) around its L
// and holds its error inside that pair. Any other target takes the nearest of all entries to the
// target plus its carried error by the weighted Oklab distance. A blue noise threshold moves the
// decision between the two nearest entries. The carried error is in linear light: target plus
// carried error minus the chosen entry, diffused to the neighbours by the kernel set in tones.cpp.
// The error rows persist from row to row. alloc() clears them for the next window.
struct tone_dither
{
    int      w     = 0;
    int16_t* block = nullptr; // the three rows in one allocation, internal RAM when it fits
    int16_t* rows[3] = {nullptr, nullptr, nullptr}; // (w + 4) x 3 q15 each, index x + 2
    tone_dither() = default;
    tone_dither(const tone_dither&) = delete;
    tone_dither& operator=(const tone_dither&) = delete;
    ~tone_dither() { release(); }
    void alloc(int w_);
    void release();
    // One RGB row (3 bytes per pixel, w pixels) to w entry indices at out. Without memory for the
    // error rows the row is paper.
    void row(const uint8_t* rgb, int y, uint8_t* out);
};

#if TONES_HAVE_FLOAT_REF
// The float dither the fixed point one is checked and timed against: the same rules in 32 bit
// float with libm, the target of each RGB colour kept in a cache, and the coloured search over
// the nearest neighbours of the target's nearest entry (tones_float_ref.inc).
struct tone_dither_float
{
    int w = 0;
    std::vector<float> rows[3]; // (w + 4) x 3 floats each, index x + 2
    void alloc(int w_);
    void release();
    void row(const uint8_t* rgb, int y, uint8_t* out);
};

// tones_solid_gray() by the float targets.
int tones_float_ref_solid_gray(int g);
#endif

struct tone_draw_stats
{
    uint32_t pulls1   = 0; // phase 1 pulls played
    uint32_t pulls2   = 0; // phase 2 pulls played
    uint32_t sends    = 0; // pulls with a plane send
    uint32_t open_ms  = 0; // the session opens of both phases
    uint32_t pull_ms  = 0; // summed BUSY time of the pulls
    uint32_t total_ms = 0; // the whole call
    uint32_t entries  = 0; // distinct entries in the frame
};

// Draw the frame onto the panel: the plan of the frame's entries (tone_plan_build), then every pull
// of phase 1 on its session and every pull of phase 2 on its session, each pixel playing its entry's
// code. Closes the session and clears the planes at the end. On a panel without the tables the
// frame is reduced to the three stock colours and refreshed with the panel's own waveform. False on
// a driver error.
bool tones_draw(const uint8_t* frame, tone_draw_stats* st);

#ifdef TONES_BENCH
// Times the tone code and prints lines that start with BENCH, the last one BENCH DONE. Does not
// touch the panel. Call after tones_init().
void tones_bench();
#endif
