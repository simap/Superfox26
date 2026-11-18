// BMP and GIF image decoders, one of the per-format decoder sets reached through decode_image in
// image.h.
// A decoder clamps the requested size to the source size and never enlarges the image, and it
// rejects any source whose pixel count is over a fixed limit.
#pragma once

#include <string>

#include "image.h"

bool sniff_bmp(const std::string& data, int& w, int& h);

bool decode_gif(const std::string& data, gimage& img, int dw, int dh);
bool decode_bmp(const std::string& data, gimage& img, int dw, int dh);
