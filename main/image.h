// Image header sniffing and decoding to a display size.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One decoded copy of an image at (at most) one display size: 24-bit RGB plus an optional 8-bit alpha.
struct gimage
{
    int                  req_w = 0, req_h = 0; // display size this copy was decoded for
    int                  w = 0, h = 0;         // stored size: min(display, natural) per axis
    std::vector<uint8_t> px;    // RGB, 3 bytes per pixel, row-major
    std::vector<uint8_t> alpha; // 1 byte per pixel, row-major; empty if opaque
    int                  last_y = 0; // lowest page row (exclusive) any draw so far needs
    long                 last_use = 0;
    size_t  bytes() const { return px.capacity() + alpha.capacity(); }
    uint8_t alpha_at(size_t o) const { return alpha.empty() ? 255 : alpha[o]; }
};

enum { IMG_NONE = 0, IMG_PNG = 1, IMG_JPEG = 2, IMG_GIF = 3, IMG_WEBP = 4, IMG_BMP = 5 };

// Natural size from the file header. Returns the kind, IMG_NONE if unknown or unreadable. data may be
// a prefix of the file: when the format is recognised but the size lies beyond the end of data (a JPEG
// whose frame header follows large EXIF or ICC blocks), *incomplete is set and IMG_NONE returned.
int sniff_size(const std::string& data, int& w, int& h, bool* incomplete = nullptr);

// Decode data (a whole file) to dw x dh, both already clamped to the natural size. PNG, JPEG, GIF
// (first frame) and BMP go through the box filter in downsample.h, WebP through libwebp's rescaler
// (image_webp.h). False on a decoder error or when the decode would not fit in PSRAM; img is then
// empty.
bool decode_image(int kind, const std::string& data, gimage& img, int dw, int dh);
