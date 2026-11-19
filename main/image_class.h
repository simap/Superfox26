// Whether an image is a photo, which the dither renders by blue noise, or a graphic, which it
// renders by error diffusion. render.cpp decides once per image source. host_tests/image_class
// checks it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "image.h"

static const int IMAGE_CLASS_SMALL   = 48;
static const int IMAGE_CLASS_SAMPLES = 4096;
static const int IMAGE_CLASS_FLAT    = 40;
static const int IMAGE_CLASS_COLOURS = 192;

struct image_class
{
    bool photo = false;
    char why[64] = "";
};

// kind and the native size w x h of the source, lossy for a WebP in the lossy form, and one decode
// of it: px three bytes per pixel, alpha one or empty, pw x ph. Under IMAGE_CLASS_SMALL px on a side
// is a graphic, then JPEG and lossy WebP are photos. Else up to IMAGE_CLASS_SAMPLES opaque pixels on
// a grid decide: a photo has few equal right neighbours (below IMAGE_CLASS_FLAT percent) and many
// colours (IMAGE_CLASS_COLOURS, or one in 8 samples when fewer).
inline image_class image_classify(int kind, bool lossy, int w, int h, const uint8_t* px, const uint8_t* alpha,
                                  int pw, int ph)
{
    image_class c;
    if(w < IMAGE_CLASS_SMALL || h < IMAGE_CLASS_SMALL)
    {
        snprintf(c.why, sizeof(c.why), "%dx%d, under %d px", w, h, IMAGE_CLASS_SMALL);
        return c;
    }
    if(kind == IMG_JPEG || (kind == IMG_WEBP && lossy))
    {
        c.photo = true;
        snprintf(c.why, sizeof(c.why), "%s", kind == IMG_JPEG ? "JPEG" : "lossy WebP");
        return c;
    }
    int step = 1;
    while(static_cast<int64_t>(pw / step) * (ph / step) > IMAGE_CLASS_SAMPLES) step++;
    const unsigned        SLOTS = 2 * IMAGE_CLASS_SAMPLES;
    std::vector<uint32_t> seen(SLOTS, 0);
    int                   n = 0, flat = 0, colours = 0;
    for(int y = step / 2; y < ph; y += step)
        for(int x = step / 2; x + 1 < pw; x += step)
        {
            const size_t o = static_cast<size_t>(y) * pw + x;
            if(alpha && alpha[o] != 255) continue;
            const uint8_t* p   = px + 3 * o;
            const uint32_t rgb = (static_cast<uint32_t>(p[0]) << 16 | p[1] << 8 | p[2]) + 1;
            n++;
            flat += p[0] == p[3] && p[1] == p[4] && p[2] == p[5] && (!alpha || alpha[o + 1] == 255);
            for(unsigned i = (rgb * 2654435761u) % SLOTS;; i = (i + 1) % SLOTS)
            {
                if(seen[i] == rgb) break;
                if(seen[i] == 0)
                {
                    seen[i] = rgb;
                    colours++;
                    break;
                }
            }
        }
    const int need = n / 8 < IMAGE_CLASS_COLOURS ? n / 8 : IMAGE_CLASS_COLOURS;
    c.photo        = n >= 64 && flat * 100 < IMAGE_CLASS_FLAT * n && colours >= need;
    snprintf(c.why, sizeof(c.why), "%d colours and %d%% flat in %d samples", colours, n ? flat * 100 / n : 0, n);
    return c;
}
