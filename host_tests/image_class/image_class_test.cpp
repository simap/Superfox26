// Host test of main/image_class.h: the photo class of an image by size, format and a sample of its
// pixels. Usage: image_class_test. Exit status 0 when every check passes.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "image_class.h"

namespace
{
unsigned s_fails = 0;

uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

struct pic
{
    int                  w, h;
    std::vector<uint8_t> px, alpha;
    pic(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 3, 255) {}
    void put(int x, int y, int r, int g, int b)
    {
        uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 3];
        p[0] = static_cast<uint8_t>(r), p[1] = static_cast<uint8_t>(g), p[2] = static_cast<uint8_t>(b);
    }
};

int clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void check(const char* name, int kind, bool lossy, const pic& p, bool want)
{
    const image_class c = image_classify(kind, lossy, p.w, p.h, p.px.data(), p.alpha.empty() ? nullptr : p.alpha.data(),
                                         p.w, p.h);
    const bool ok = c.photo == want;
    printf("%s  %-34s %-7s (%s)\n", ok ? "pass" : "FAIL", name, c.photo ? "photo" : "graphic", c.why);
    if(!ok) s_fails++;
}

// A smooth two way gradient with noise of up to n steps.
pic gradient(int w, int h, int n)
{
    pic p(w, h);
    for(int y = 0; y < h; y++)
        for(int x = 0; x < w; x++)
        {
            const int d = n ? static_cast<int>(mix32(static_cast<uint32_t>(y * w + x)) % (2 * n + 1)) - n : 0;
            p.put(x, y, clamp8(40 + 180 * x / w + d), clamp8(30 + 120 * y / h + d), clamp8(90 + d));
        }
    return p;
}
} // namespace

int main()
{
    check("noisy gradient 32x32 PNG", IMG_PNG, false, gradient(32, 32, 8), false);
    check("noisy gradient 300x40 PNG", IMG_PNG, false, gradient(300, 40, 8), false);
    check("flat 40x40 JPEG", IMG_JPEG, false, pic(40, 40), false);
    check("flat 200x200 JPEG", IMG_JPEG, false, pic(200, 200), true);
    check("flat 200x200 lossy WebP", IMG_WEBP, true, pic(200, 200), true);
    check("flat 200x200 lossless WebP", IMG_WEBP, false, pic(200, 200), false);
    check("noisy gradient 400x300 PNG", IMG_PNG, false, gradient(400, 300, 8), true);
    check("noisy gradient 400x300 lossless WebP", IMG_WEBP, false, gradient(400, 300, 8), true);
    check("plain gradient 400x300 PNG", IMG_PNG, false, gradient(400, 300, 0), false);

    pic bars(400, 300);
    for(int y = 0; y < 300; y++)
        for(int x = 0; x < 400; x++)
        {
            const int k = (x / 40 + y / 60) % 5;
            bars.put(x, y, 50 * k, 200 - 40 * k, 120);
        }
    check("five colour bars 400x300 PNG", IMG_PNG, false, bars, false);

    pic palette(400, 300), few(400, 300);
    for(int y = 0; y < 300; y++)
        for(int x = 0; x < 400; x++)
        {
            const uint32_t v = mix32(static_cast<uint32_t>(y * 400 + x));
            palette.put(x, y, static_cast<int>(v % 16) * 16, static_cast<int>(v / 16 % 16) * 16, 128);
            few.put(x, y, static_cast<int>(v % 6) * 40, static_cast<int>(v / 6 % 6) * 40, 128);
        }
    check("256 colour noise 400x300 GIF", IMG_GIF, false, palette, true);
    check("36 colour noise 400x300 GIF", IMG_GIF, false, few, false);

    pic icon = gradient(200, 200, 8);
    icon.alpha.assign(200 * 200, 0);
    for(int y = 90; y < 95; y++)
        for(int x = 90; x < 95; x++) icon.alpha[static_cast<size_t>(y) * 200 + x] = 255;
    check("200x200 PNG, opaque only in a 5x5 spot", IMG_PNG, false, icon, false);
    pic cut = gradient(400, 300, 8);
    cut.alpha.assign(400 * 300, 255);
    for(int y = 0; y < 300; y++)
        for(int x = 0; x < 100; x++) cut.alpha[static_cast<size_t>(y) * 400 + x] = 0;
    check("noisy 400x300 PNG with a clear quarter", IMG_PNG, false, cut, true);

    printf("%s\n", s_fails ? "FAIL" : "PASS");
    return s_fails ? 1 : 0;
}
