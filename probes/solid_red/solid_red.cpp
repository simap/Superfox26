// Host probe: which tone entries the fixed point dither (main/tones.cpp tone_dither) gives a solid
// sRGB fill. For each colour it dithers a 240 x 120 block and prints the share of each entry used.
// Usage: out/solid_red [R G B ...]. With no arguments it runs (v, 0, 0) for v from 200 to 255,
// then 226 57 22, the red reference entry's sRGB.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "tonemap.h"
#include "tones.h"

static const int W = 240, H = 120;

static void probe(int r, int g, int b)
{
    tone_dither q;
    q.alloc(W);
    std::vector<uint8_t> rgb(W * 3), out(W);
    for(int x = 0; x < W; x++)
    {
        rgb[x * 3]     = static_cast<uint8_t>(r);
        rgb[x * 3 + 1] = static_cast<uint8_t>(g);
        rgb[x * 3 + 2] = static_cast<uint8_t>(b);
    }
    std::vector<int> n(TONEMAP_COUNT, 0);
    for(int y = 0; y < H; y++)
    {
        q.row(rgb.data(), y, out.data());
        for(int x = 0; x < W; x++) n[out[x]]++;
    }
    int used = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++) used += n[e] ? 1 : 0;
    printf("%3d %3d %3d  #%02x%02x%02x  %d entries:", r, g, b, r, g, b, used);
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(n[e])
            printf("  e%u %.1f%% (L %.4f a %+.4f b %+.4f %s)", e, 100.0 * n[e] / (W * H), k_tonemap[e].L,
                   k_tonemap[e].a, k_tonemap[e].b, k_tonemap[e].pattern);
    printf("\n");
}

int main(int argc, char** argv)
{
    if(!tones_init()) return 1;
    if(argc >= 4)
    {
        for(int i = 1; i + 2 < argc; i += 3) probe(atoi(argv[i]), atoi(argv[i + 1]), atoi(argv[i + 2]));
        return 0;
    }
    for(int v = 200; v <= 255; v++) probe(v, 0, 0);
    probe(226, 57, 22);
    return 0;
}
