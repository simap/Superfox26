// Maps a URL to the kind, width and height of its image, so a caller learns an image's size without
// fetching and decoding it.
// The table lives in RTC memory and is kept across a reset.
// Call sizecache_init before the first sizecache_get or sizecache_put.
// A URL is identified only by a hash, so sizecache_get can report the size stored for a different
// URL when two URLs hash alike.
#pragma once

#include <string>

void sizecache_init(bool kept);

bool sizecache_get(const std::string& url, int& kind, int& w, int& h);

void sizecache_put(const std::string& url, int kind, int w, int h);
