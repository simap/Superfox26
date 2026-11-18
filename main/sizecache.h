// Natural image sizes by URL, kept across pages and across deep sleep so a page seen before needs no
// header fetch for layout. The table is 128 sets of 4 entries of 12 bytes, 6144 bytes of RTC slow memory
// that is not initialised at boot. Deep sleep, a software reset and a panic restart keep it. The magic
// word of rtcstate.h tells whether it is valid. An entry holds the 64 bit FNV-1a hash of the URL, kind,
// w and h. The low 7 bits of the hash select the set and hold kind in the entry. A put into a full set
// replaces a random entry of that set. A put writes the entry so that a reset during the put leaves it
// empty or complete. Failures are not cached. A mismatch found at draw time replaces the entry.
// Browser task only.
#pragma once

#include <string>

// Clears the table when kept is false, the result of rtcstate_begin(). Reads nothing when kept is true.
void sizecache_init(bool kept);

// True with kind, w and h when url is known.
bool sizecache_get(const std::string& url, int& kind, int& w, int& h);

// Ignored unless w and h are 1 to 65535 and kind is 1 to 127.
void sizecache_put(const std::string& url, int kind, int w, int h);
