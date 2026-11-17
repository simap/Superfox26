// WebP decoder: libwebp's advanced API decodes straight into img.px at the display size. libwebp's own
// rescaler (an area average, like the box filter in downsample.h) does the reduction while it decodes, so
// no natural-size RGB copy exists. Opaque files decode as RGB into img.px; files with alpha decode as RGBA
// into img.px and are then packed to RGB in place, with alpha split out to img.alpha as the downsampler
// does at 1:1.
//
// Memory per decode, n = dw * dh, N = natural width * height:
//   opaque lossy:  3n (img.px) + libwebp state of a few macroblock rows
//   lossy + alpha: 4n (img.px, capacity stays 4n) + n (img.alpha) + N (alpha plane) + the lossy state
//   lossless:      3n or 4n + about 4N (libwebp keeps the whole ARGB image) + Huffman tables

#include "image_webp.h"

#include <cstring>

#include "webp/decode.h"
#include "esp_log.h"

static const char* TAG = "image_webp";

bool sniff_webp(const std::string& data, int& w, int& h)
{
    auto u = reinterpret_cast<const uint8_t*>(data.data());
    if(data.size() < 20 || memcmp(u, "RIFF", 4) || memcmp(u + 8, "WEBP", 4)) return false;
    int iw = 0, ih = 0;
    if(!WebPGetInfo(u, data.size(), &iw, &ih)) return false;
    w = iw;
    h = ih;
    return true;
}

namespace
{
void clear(gimage& img)
{
    std::vector<uint8_t>().swap(img.px);
    std::vector<uint8_t>().swap(img.alpha);
}

// RGBA (4 bytes per pixel) to RGB in the same buffer, plus img.alpha. Same result as the downsampler
// with a 1:1 scale: color as decoded where alpha > 0, white where alpha is 0. Returns false when every
// pixel is opaque.
bool pack_rgba(gimage& img)
{
    size_t   n = static_cast<size_t>(img.w) * img.h;
    uint8_t* p = img.px.data();
    bool     transparent = false;
    img.alpha.resize(n);
    for(size_t o = 0; o < n; o++)
    {
        uint8_t* s = p + 4 * o;
        uint8_t  r = s[0], g = s[1], b = s[2], a = s[3];
        uint8_t* d = p + 3 * o; // d <= s: pixel o is read before anything at or after it is written
        if(a == 0) r = g = b = 255;
        d[0]         = r;
        d[1]         = g;
        d[2]         = b;
        img.alpha[o] = a;
        if(a < 255) transparent = true;
    }
    img.px.resize(n * 3);
    return transparent;
}
} // namespace

bool decode_webp(const std::string& data, gimage& img, int dw, int dh)
{
    auto              u = reinterpret_cast<const uint8_t*>(data.data());
    WebPDecoderConfig cfg;
    clear(img);
    if(!WebPInitDecoderConfig(&cfg)) return false;
    VP8StatusCode st = WebPGetFeatures(u, data.size(), &cfg.input);
    if(st != VP8_STATUS_OK)
    {
        ESP_LOGW(TAG, "webp: header error %d", static_cast<int>(st));
        return false;
    }
    if(cfg.input.has_animation)
    {
        ESP_LOGW(TAG, "webp: animation not supported");
        return false;
    }
    int sw = cfg.input.width, sh = cfg.input.height;
    if(dw < 1 || dh < 1 || dw > sw || dh > sh)
    {
        ESP_LOGW(TAG, "webp: bad size %dx%d for %dx%d", dw, dh, sw, sh);
        return false;
    }
    bool   alpha = cfg.input.has_alpha != 0;
    int    bpp   = alpha ? 4 : 3;
    size_t n     = static_cast<size_t>(dw) * dh;
    img.w        = dw;
    img.h        = dh;
    img.px.resize(n * bpp);

    if(dw != sw || dh != sh)
    {
        cfg.options.use_scaling   = 1;
        cfg.options.scaled_width  = dw;
        cfg.options.scaled_height = dh;
    }
    cfg.output.colorspace         = alpha ? MODE_RGBA : MODE_RGB;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba        = img.px.data();
    cfg.output.u.RGBA.stride      = dw * bpp;
    cfg.output.u.RGBA.size        = img.px.size();
    st                            = WebPDecode(u, data.size(), &cfg);
    WebPFreeDecBuffer(&cfg.output); // external memory: frees nothing, kept for symmetry with the API
    if(st != VP8_STATUS_OK)
    {
        ESP_LOGW(TAG, "webp: decode error %d", static_cast<int>(st));
        clear(img);
        return false;
    }
    if(!alpha || !pack_rgba(img)) std::vector<uint8_t>().swap(img.alpha);
    return true;
}
