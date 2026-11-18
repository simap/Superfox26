// Host probe: the fixed point dither (main/tones.cpp tone_dither) on an RGB image. Writes the result
// as a PNG with every pixel in the colour of its tone entry, and prints the share of each entry.
// Usage: out/image_dither IN.png OUT.png. IN is read as 24 bit RGB. Its alpha is dropped, so
// composite a transparent image on its background first.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lodepng.h"
#include "tonemap.h"
#include "tones.h"

static uint8_t srgb8(float v)
{
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
    return static_cast<uint8_t>(lroundf(v * 255));
}

// Oklab to sRGB, the entry as a monitor shows it.
static void entry_rgb(const tone_entry& e, uint8_t* o)
{
    float l = e.L + 0.3963377774f * e.a + 0.2158037573f * e.b;
    float m = e.L - 0.1055613458f * e.a - 0.0638541728f * e.b;
    float s = e.L - 0.0894841775f * e.a - 1.2914855480f * e.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    o[0] = srgb8(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    o[1] = srgb8(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    o[2] = srgb8(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
}

int main(int argc, char** argv)
{
    if(argc != 3)
    {
        fprintf(stderr, "usage: %s IN.png OUT.png\n", argv[0]);
        return 1;
    }
    if(!tones_init()) return 1;
    unsigned char* in = nullptr;
    unsigned       w = 0, h = 0;
    if(lodepng_decode24_file(&in, &w, &h, argv[1]))
    {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }
    tone_dither q;
    q.alloc(static_cast<int>(w));
    std::vector<uint8_t> row(w), out(static_cast<size_t>(w) * h * 3);
    std::vector<int>     n(TONEMAP_COUNT, 0);
    for(unsigned y = 0; y < h; y++)
    {
        q.row(in + static_cast<size_t>(y) * w * 3, static_cast<int>(y), row.data());
        for(unsigned x = 0; x < w; x++)
        {
            n[row[x]]++;
            entry_rgb(k_tonemap[row[x]], &out[(static_cast<size_t>(y) * w + x) * 3]);
        }
    }
    free(in);
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(n[e])
            printf("e%-3u %6.2f%%  L %.4f a %+.4f b %+.4f  %s\n", e, 100.0 * n[e] / (static_cast<double>(w) * h),
                   k_tonemap[e].L, k_tonemap[e].a, k_tonemap[e].b, k_tonemap[e].pattern);
    if(lodepng_encode24_file(argv[2], out.data(), w, h))
    {
        fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    return 0;
}
