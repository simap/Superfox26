// PNG (libpng, row by row) and JPEG (libjpeg-turbo, scale_denom then box filter) decoders. The compressed
// bytes are in memory. Decoding happens at the drawn size, so a decoded copy is never larger than what
// is blitted.

#include "image.h"
#include "downsample.h"
#include "image_legacy.h"
#include "image_webp.h"

#include <png.h>
#include <jpeglib.h>

#include <algorithm>
#include <csetjmp>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "image";

const uint8_t bayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21}};

namespace
{
uint32_t be16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
uint32_t be32(const unsigned char* p) { return (be16(p) << 16) | be16(p + 2); }

struct mem_reader
{
    const std::string* s   = nullptr;
    size_t             off = 0;
    size_t             read(void* buf, size_t n)
    {
        size_t k = std::min(n, s->size() - off);
        memcpy(buf, s->data() + off, k);
        off += k;
        return k;
    }
};

bool decode_png(const std::string& data, gimage& img, int dw, int dh)
{
    png_structp p    = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = p ? png_create_info_struct(p) : nullptr;
    if(!info)
    {
        if(p) png_destroy_read_struct(&p, nullptr, nullptr);
        return false;
    }
    mem_reader           src{&data, 0};
    std::vector<uint8_t> row, full, g, a;
    downsampler          ds;
    if(setjmp(png_jmpbuf(p)))
    {
        png_destroy_read_struct(&p, &info, nullptr);
        return false;
    }
    png_set_read_fn(p, &src, [](png_structp ps, png_bytep out, png_size_t len) {
        if(static_cast<mem_reader*>(png_get_io_ptr(ps))->read(out, len) != len) png_error(ps, "truncated");
    });
    png_read_info(p, info);
    png_set_expand(p);
    png_set_strip_16(p);
    png_set_gray_to_rgb(p);
    png_set_filler(p, 0xFF, PNG_FILLER_AFTER); // RGBA, alpha 255 where the file has none
    int passes = png_set_interlace_handling(p);
    png_read_update_info(p, info);
    int sw = static_cast<int>(png_get_image_width(p, info)), sh = static_cast<int>(png_get_image_height(p, info));
    ds.init(sw, sh, dw, dh, &img);
    g.resize(static_cast<size_t>(sw) * 3);
    a.resize(sw);
    auto push_rgba = [&](int y, const uint8_t* r) {
        for(int x = 0; x < sw; x++)
        {
            for(int k = 0; k < 3; k++) g[3 * x + k] = r[4 * x + k];
            a[x] = r[4 * x + 3];
        }
        ds.push(y, g.data(), a.data());
    };
    if(passes == 1)
    {
        row.resize(static_cast<size_t>(sw) * 4);
        for(int y = 0; y < sh; y++)
        {
            png_read_row(p, row.data(), nullptr);
            push_rgba(y, row.data());
        }
    } else
    {
        // interlaced: needs the whole image at natural size (transient)
        full.resize(static_cast<size_t>(sw) * 4 * sh);
        std::vector<png_bytep> rows(sh);
        for(int y = 0; y < sh; y++) rows[y] = &full[static_cast<size_t>(y) * sw * 4];
        png_read_image(p, rows.data());
        for(int y = 0; y < sh; y++) push_rgba(y, rows[y]);
    }
    ds.finish();
    png_read_end(p, nullptr);
    png_destroy_read_struct(&p, &info, nullptr);
    return true;
}

struct jpeg_err_mgr
{
    jpeg_error_mgr pub;
    jmp_buf        jb;
};

void jpeg_error_exit(j_common_ptr c)
{
    char msg[JMSG_LENGTH_MAX];
    (*c->err->format_message)(c, msg);
    ESP_LOGW(TAG, "jpeg: %s", msg);
    longjmp(reinterpret_cast<jpeg_err_mgr*>(c->err)->jb, 1);
}

void jpeg_quiet(j_common_ptr) {}

bool decode_jpeg(const std::string& data, gimage& img, int dw, int dh)
{
    jpeg_decompress_struct cinfo;
    jpeg_err_mgr           jerr;
    cinfo.err                 = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit       = jpeg_error_exit;
    jerr.pub.output_message   = jpeg_quiet;
    std::vector<uint8_t> row;
    downsampler          ds;
    if(setjmp(jerr.jb))
    {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(data.data()), static_cast<unsigned long>(data.size()));
    jpeg_read_header(&cinfo, TRUE);
    // largest 1/denom that still gives at least dw x dh; the downsampler does the rest
    int nw = static_cast<int>(cinfo.image_width), nh = static_cast<int>(cinfo.image_height);
    int denom = 1;
    while(denom < 8 && (nw + 2 * denom - 1) / (2 * denom) >= dw && (nh + 2 * denom - 1) / (2 * denom) >= dh)
        denom *= 2;
    cinfo.scale_num       = 1;
    cinfo.scale_denom     = static_cast<unsigned>(denom);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int sw = static_cast<int>(cinfo.output_width), sh = static_cast<int>(cinfo.output_height);
    ds.init(sw, sh, std::min(dw, sw), std::min(dh, sh), &img);
    row.resize(static_cast<size_t>(sw) * 3);
    while(cinfo.output_scanline < cinfo.output_height)
    {
        int      y = static_cast<int>(cinfo.output_scanline);
        JSAMPROW r = row.data();
        jpeg_read_scanlines(&cinfo, &r, 1);
        ds.push(y, row.data(), nullptr);
    }
    ds.finish();
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
} // namespace

static uint32_t le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static uint32_t le24(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }

// Canvas size from the first WebP chunk, so a prefix of the file is enough. WebPGetInfo would need
// the whole VP8X chunk list, which fails on a prefix when an ICC or alpha chunk comes first.
static bool webp_header_size(const unsigned char* u, size_t n, int& w, int& h)
{
    if(n < 30) return false;
    if(!memcmp(u + 12, "VP8X", 4))
    {
        w = static_cast<int>(le24(u + 24)) + 1;
        h = static_cast<int>(le24(u + 27)) + 1;
        return true;
    }
    if(!memcmp(u + 12, "VP8 ", 4))
    {
        // key frame: 3-byte frame tag at 20, start code 9d 01 2a at 23, then 14-bit width and height
        if(u[23] != 0x9d || u[24] != 0x01 || u[25] != 0x2a) return false;
        w = static_cast<int>(le16(u + 26) & 0x3fff);
        h = static_cast<int>(le16(u + 28) & 0x3fff);
        return w > 0 && h > 0;
    }
    if(!memcmp(u + 12, "VP8L", 4))
    {
        if(u[20] != 0x2f) return false;
        uint32_t b = u[21] | (u[22] << 8) | (u[23] << 16) | (static_cast<uint32_t>(u[24]) << 24);
        w          = static_cast<int>((b & 0x3fff) + 1);
        h          = static_cast<int>(((b >> 14) & 0x3fff) + 1);
        return true;
    }
    return false;
}

int sniff_size(const std::string& data, int& w, int& h, bool* incomplete)
{
    if(incomplete) *incomplete = false;
    auto   u = reinterpret_cast<const unsigned char*>(data.data());
    size_t n = data.size();
    if(n > 24 && u[0] == 0x89 && u[1] == 'P' && !memcmp(u + 12, "IHDR", 4))
    {
        w = static_cast<int>(be32(u + 16));
        h = static_cast<int>(be32(u + 20));
        return IMG_PNG;
    }
    if(n > 4 && u[0] == 0xFF && u[1] == 0xD8)
    {
        size_t o = 2;
        while(o + 9 < n)
        {
            if(u[o] != 0xFF)
            {
                o++;
                continue;
            }
            int m = u[o + 1];
            if(m == 0xFF)
            {
                o++;
                continue;
            }
            if(m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            {
                o += 2;
                continue;
            }
            if(m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            {
                h = static_cast<int>(be16(u + o + 5));
                w = static_cast<int>(be16(u + o + 7));
                return IMG_JPEG;
            }
            o += 2 + be16(u + o + 2);
        }
        if(incomplete) *incomplete = true; // the frame header lies past the end of data
        return IMG_NONE;
    }
    if(n > 10 && !memcmp(u, "GIF8", 4))
    {
        w = u[6] | (u[7] << 8);
        h = u[8] | (u[9] << 8);
        return IMG_GIF;
    }
    if(n > 12 && !memcmp(u, "RIFF", 4) && !memcmp(u + 8, "WEBP", 4))
    {
        if(webp_header_size(u, n, w, h) || sniff_webp(data, w, h)) return IMG_WEBP;
        if(incomplete && n < 30) *incomplete = true;
        return IMG_NONE;
    }
    if(n > 26 && u[0] == 'B' && u[1] == 'M') return sniff_bmp(data, w, h) ? IMG_BMP : IMG_NONE;
    return IMG_NONE;
}

// libwebp keeps the whole image at its natural size while decoding: measured 6.4 bytes per natural
// pixel for lossy with alpha and 4.2 for lossless, plus the output. A decode that would not leave
// 1 MB of PSRAM is skipped, since an allocation failure aborts the firmware.
static bool webp_fits(const std::string& data, int dw, int dh)
{
    int nw = 0, nh = 0;
    if(!sniff_webp(data, nw, nh)) return false;
    size_t need = static_cast<size_t>(nw) * nh * 7 + static_cast<size_t>(dw) * dh * 4 + 1024 * 1024;
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if(free_ps < need)
        ESP_LOGW(TAG, "webp %dx%d skipped: needs about %u, free PSRAM %u", nw, nh, (unsigned)need, (unsigned)free_ps);
    return free_ps >= need;
}

bool decode_image(int kind, const std::string& data, gimage& img, int dw, int dh)
{
    bool ok = false;
    if(kind == IMG_PNG) ok = decode_png(data, img, dw, dh);
    else if(kind == IMG_JPEG) ok = decode_jpeg(data, img, dw, dh);
    else if(kind == IMG_WEBP) ok = webp_fits(data, dw, dh) && decode_webp(data, img, dw, dh);
    else if(kind == IMG_GIF) ok = decode_gif(data, img, dw, dh);
    else if(kind == IMG_BMP) ok = decode_bmp(data, img, dw, dh);
    if(!ok)
    {
        std::vector<uint8_t>().swap(img.px);
        std::vector<uint8_t>().swap(img.mask);
    }
    return ok;
}
