// Decodes a compressed image, in PNG, JPEG, GIF, WEBP or BMP form, into a plain RGB buffer the
// renderer can sample.
// A caller first passes the data to sniff_size for the format kind and native size, then gives that
// kind to decode_image.
// decode_image scales the result to the dw by dh target, so gimage w and h hold that target size,
// not the native size.
// gimage px holds three bytes per pixel in red, green and blue order, and alpha_at reads the
// opacity, which is fully opaque wherever alpha is empty.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct gimage
{
    int                  req_w = 0, req_h = 0;
    int                  w = 0, h = 0;
    std::vector<uint8_t> px;
    std::vector<uint8_t> alpha;
    int                  last_y = 0;
    long                 last_use = 0;
    size_t  bytes() const { return px.capacity() + alpha.capacity(); }
    uint8_t alpha_at(size_t o) const { return alpha.empty() ? 255 : alpha[o]; }
};

enum { IMG_NONE = 0, IMG_PNG = 1, IMG_JPEG = 2, IMG_GIF = 3, IMG_WEBP = 4, IMG_BMP = 5 };

int sniff_size(const std::string& data, int& w, int& h, bool* incomplete = nullptr);

bool decode_image(int kind, const std::string& data, gimage& img, int dw, int dh);
