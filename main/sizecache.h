// Natural image sizes by URL, kept across pages so a page seen before needs no header fetch for
// layout. 512 entries of 24 bytes in PSRAM, least recently used eviction, saved to the data
// partition after a page that changed it. Failures are not cached; a mismatch found at draw time
// replaces the entry. Browser task only.
#pragma once

#include <string>

// Loads /data/imgsize.bin if present. Call after cfg_init.
void sizecache_init();

// True with kind, w and h when url is known.
bool sizecache_get(const std::string& url, int& kind, int& w, int& h);

void sizecache_put(const std::string& url, int kind, int w, int h);

// Writes the file when entries changed since the last save. Call after a page.
void sizecache_save();

// Entries held.
int sizecache_count();
