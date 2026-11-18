// Renders a full page to the red, black and white EPD panel.
// Each pixel is one tone-map entry index, and the panel's tone tables are driven over several
// frames to reach more tones than a single frame shows.
// tone_dither turns an sRGB row into such entry indices by error diffusion.
// A frame is TONE_W by TONE_H bytes, one tone-map entry per pixel, with TONE_PAPER as the
// background.
// Call tones_init once before tones_draw or any tone_dither::row.
// All tone_dither::row calls run through one shared static scratch area, so do not run two at once,
// even on separate tone_dither objects.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

static const int TONE_W = 960;
static const int TONE_H = 672;

static const uint8_t TONE_PAPER = 0;

#if defined(TONES_BENCH) || defined(TONES_FLOAT_REF)
#define TONES_HAVE_FLOAT_REF 1
#else
#define TONES_HAVE_FLOAT_REF 0
#endif

bool tones_init();

int tones_solid_gray(int g);

uint8_t* tones_frame();

struct tone_dither
{
    int      w     = 0;
    int16_t* block = nullptr;
    int16_t* rows[3] = {nullptr, nullptr, nullptr};
    tone_dither() = default;
    tone_dither(const tone_dither&) = delete;
    tone_dither& operator=(const tone_dither&) = delete;
    ~tone_dither() { release(); }
    void alloc(int w_);
    void release();
    void row(const uint8_t* rgb, int y, uint8_t* out);
};

#if TONES_HAVE_FLOAT_REF
struct tone_dither_float
{
    int w = 0;
    std::vector<float> rows[3];
    void alloc(int w_);
    void release();
    void row(const uint8_t* rgb, int y, uint8_t* out);
};

int tones_float_ref_solid_gray(int g);
#endif

struct tone_draw_stats
{
    uint32_t pulls1   = 0;
    uint32_t pulls2   = 0;
    uint32_t sends    = 0;
    uint32_t open_ms  = 0;
    uint32_t pull_ms  = 0;
    uint32_t total_ms = 0;
    uint32_t entries  = 0;
};

bool tones_draw(const uint8_t* frame, tone_draw_stats* st);

#ifdef TONES_BENCH
void tones_bench();
#endif
