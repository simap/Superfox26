// WebP sniffing and decoding (libwebp, components/webp): lossy, lossless, alpha. Not animation.
#pragma once

#include <string>

#include "image.h"

// True for a RIFF/WEBP file whose VP8, VP8L or VP8X header gives a size; w x h is the natural (canvas) size.
// Also true for an animated file, which decode_webp refuses.
bool sniff_webp(const std::string& data, int& w, int& h);

// Decode data (a whole WebP file) to dw x dh, both already clamped to the natural size. Fills img.px (RGB) and
// img.alpha (empty when every pixel is opaque) like decode_png. False on a decoder error; img is then empty.
bool decode_webp(const std::string& data, gimage& img, int dw, int dh);
