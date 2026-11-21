// Host probe: the fixed point dither (main/tones.cpp tone_dither) on an RGB image. Writes the result
// as a PNG with every pixel in the colour of its tone entry, and prints the share of each entry and
// the dither time.
// Usage: out/image_dither [--photo] [--mode diffuse|triangle|nearest] IN.png OUT.png. IN is read as
// 24 bit RGB. Its alpha is dropped, so composite a transparent image on its background first.
// --photo marks every pixel as a photo pixel, so the image takes the dither of the photo mode that
// --mode names (tones.h), TONE_PHOTO_DEFAULT without it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    const char* const modes[TONE_PHOTO_MODES] = {"diffuse", "triangle", "nearest"};
    bool              photo = false, ok = argc >= 3;
    int               a     = 1;
    for(; ok && a < argc - 2; a++)
    {
        if(strcmp(argv[a], "--photo") == 0) photo = true;
        else if(strcmp(argv[a], "--mode") == 0 && a + 1 < argc - 2)
        {
            int m = 0;
            while(m < TONE_PHOTO_MODES && strcmp(argv[a + 1], modes[m]) != 0) m++;
            ok = m < TONE_PHOTO_MODES;
            if(ok) tones_set_photo_mode(static_cast<tone_photo_mode>(m)), a++;
        }
        else ok = false;
    }
    if(!ok)
    {
        fprintf(stderr, "usage: %s [--photo] [--mode diffuse|triangle|nearest] IN.png OUT.png\n", argv[0]);
        return 1;
    }
    const char* in_path  = argv[argc - 2];
    const char* out_path = argv[argc - 1];
    if(!tones_init()) return 1;
    unsigned char* in = nullptr;
    unsigned       w = 0, h = 0;
    if(lodepng_decode24_file(&in, &w, &h, in_path))
    {
        fprintf(stderr, "%s: cannot read\n", in_path);
        return 1;
    }
    tone_dither q;
    q.alloc(static_cast<int>(w));
    std::vector<uint8_t> idx(static_cast<size_t>(w) * h), mask(w, photo ? 1 : 0), out(static_cast<size_t>(w) * h * 3);
    std::vector<int>     n(TONEMAP_COUNT, 0);
    const auto           t0 = std::chrono::steady_clock::now();
    for(unsigned y = 0; y < h; y++)
        q.row(in + static_cast<size_t>(y) * w * 3, static_cast<int>(y), &idx[static_cast<size_t>(y) * w],
              photo ? mask.data() : nullptr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for(size_t p = 0; p < idx.size(); p++)
    {
        n[idx[p]]++;
        entry_rgb(k_tonemap[idx[p]], &out[p * 3]);
    }
    free(in);
    printf("%s, %ux%u, dither %.1f ms\n", photo ? modes[q.mode] : "error diffusion", w, h, ms);
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(n[e])
            printf("e%-3u %6.2f%%  L %.4f a %+.4f b %+.4f  %s\n", e, 100.0 * n[e] / (static_cast<double>(w) * h),
                   k_tonemap[e].L, k_tonemap[e].a, k_tonemap[e].b, k_tonemap[e].pattern);
    if(lodepng_encode24_file(out_path, out.data(), w, h))
    {
        fprintf(stderr, "%s: cannot write\n", out_path);
        return 1;
    }
    return 0;
}
