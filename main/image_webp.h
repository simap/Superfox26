// WebP backend for the image module.
// Detects WebP data and decodes it into a gimage for the sniff_size and decode_image dispatchers
// declared in image.h.
#pragma once

#include <string>

#include "image.h"

bool sniff_webp(const std::string& data, int& w, int& h);

bool decode_webp(const std::string& data, gimage& img, int dw, int dh);

// True for WebP data in the lossy form, with or without alpha.
bool webp_lossy(const std::string& data);
