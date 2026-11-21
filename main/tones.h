// Renders a full page to the red, black and white EPD panel.
// Each pixel is one tone-map entry index, and the panel's tone tables are driven over several
// frames to reach more tones than a single frame shows.
// tone_dither turns an sRGB row into such entry indices: Atkinson error diffusion, and for the
// pixels a mask row marks as photo an ordered dither by blue noise that carries no error, as the
// photo mode sets.
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

// The dither of the pixels a mask marks as photo. A neutral photo pixel mixes the two neutral
// entries around it in every mode but TONE_PHOTO_DIFFUSE.
// TONE_PHOTO_DIFFUSE: the mask is ignored and every pixel takes error diffusion.
// TONE_PHOTO_TRIANGLE: a coloured photo pixel mixes the three entries that enclose its target in
// linear light, so the mix keeps the mean colour, even with entries far apart.
// TONE_PHOTO_NEAREST: a coloured photo pixel mixes up to three entries nearest its target that
// touch each other (tone_photo.h). The colour can be off where no nearer entries enclose it.
enum tone_photo_mode : uint8_t
{
    TONE_PHOTO_DIFFUSE,
    TONE_PHOTO_TRIANGLE,
    TONE_PHOTO_NEAREST,
    TONE_PHOTO_MODES,
};

static const tone_photo_mode TONE_PHOTO_DEFAULT = TONE_PHOTO_NEAREST;

// The mode tone_dither::alloc() takes for the dither of a page. On the badge it is kept across deep
// sleep, and a power up starts at TONE_PHOTO_DEFAULT.
void            tones_set_photo_mode(tone_photo_mode m);
tone_photo_mode tones_photo_mode();

struct tone_dither
{
    int             w       = 0;
    int16_t*        block   = nullptr;
    int16_t*        rows[3] = {nullptr, nullptr, nullptr};
    tone_photo_mode mode    = TONE_PHOTO_DEFAULT;
    tone_dither() = default;
    tone_dither(const tone_dither&) = delete;
    tone_dither& operator=(const tone_dither&) = delete;
    ~tone_dither() { release(); }
    // Takes the photo mode of tones_photo_mode().
    void alloc(int w_);
    void release();
    // mask holds one byte per pixel, not 0 for a photo pixel, or is null for none.
    void row(const uint8_t* rgb, int y, uint8_t* out, const uint8_t* mask = nullptr);
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
