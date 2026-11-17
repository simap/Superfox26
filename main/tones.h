// Tone rendering: RGB pixels to tone map entries by Oklab error diffusion, and the entries' pull
// patterns played onto the panel. The tone map is main/tonemap.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

static const int TONE_W = 960;
static const int TONE_H = 672;

// Entry index of the paper tone. The frame buffer is cleared to it.
static const uint8_t TONE_PAPER = 0;

// Precompute the entries' Oklab coordinates, the mapping table and each entry's neighbours, and
// allocate the frame. Call once after epd_begin(). False when the frame does not fit in PSRAM.
bool tones_init();

// Entry index of the panel black (pattern iiiii).
uint8_t tones_black();

// The gray byte (r = g = b) nearest to g whose mapped tone lands on a neutral entry, so an area filled
// with it renders solid, without dither. For UI fills such as the tab bar.
int tones_solid_gray(int g);

// The frame: TONE_W x TONE_H entry indices, row major.
uint8_t* tones_frame();

// Error-diffusion dither of RGB rows into entry indices, in Oklab, serpentine. Per pixel the RGB is
// folded onto the paper, black, red triangle (ink = 255 - max, red = max(0, R - max(G, B))), the
// barycentric Oklab target is formed, the carried error added, the nearest entry picked by the
// weighted Oklab distance with a blue noise threshold between the two nearest, and the residual
// diffused to the neighbours by the kernel set in tones.cpp (Stucki). A pixel with red below
// TONEMAP_GRAY_MAX_RED takes only neutral entries and carries only lightness error, so chroma never
// tints a gray. The error rows persist from row to row. alloc() clears them for a new window.
struct tone_dither
{
    int w = 0;
    std::vector<float> rows[3]; // (w + 4) x 3 floats each, index x + 2
    void alloc(int w_);
    void release();
    // One RGB row (3 bytes per pixel, w pixels) to w entry indices at out.
    void row(const uint8_t* rgb, int y, uint8_t* out);
};

struct tone_draw_stats
{
    uint32_t erase_ms = 0;  // the erase pull, BUSY low to high
    uint32_t open_ms  = 0;  // seq session open after the erase
    uint32_t steps    = 0;  // pulls played
    uint32_t sends    = 0;  // pulls with a plane send
    uint32_t pull_ms  = 0;  // summed BUSY time of the pulls
    uint32_t total_ms = 0;  // the whole call
    uint32_t entries  = 0;  // distinct entries in the frame
};

// Draw the frame onto the panel: erase, then one seq pull per pattern step with every pixel playing
// its entry's pattern in lockstep. On a panel without the seq tables the frame is reduced to the
// three stock colours and refreshed with the panel's own waveform. False on a driver error.
bool tones_draw(const uint8_t* frame, tone_draw_stats* st);
