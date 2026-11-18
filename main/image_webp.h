// WebP backend for the image module.
// Detects WebP data and decodes it into a gimage for the sniff_size and decode_image dispatchers
// declared in image.h.
#pragma once

#include <string>

#include "image.h"

bool sniff_webp(const std::string& data, int& w, int& h);

bool decode_webp(const std::string& data, gimage& img, int dw, int dh);
