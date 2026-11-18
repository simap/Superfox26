#include "sizecache.h"

#include <cstdint>
#include <cstring>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"

static const char*    TAG       = "sizecache";
static const int      SETS      = 128;
static const int      WAYS      = 4;
static const uint32_t KIND_MASK = SETS - 1; // the set index bits of the hash, which hold kind in an entry

static_assert((SETS & (SETS - 1)) == 0 && (WAYS & (WAYS - 1)) == 0, "SETS and WAYS are powers of 2");

namespace
{
// key is the 64 bit FNV-1a hash of the URL with its low 7 bits, the set index, replaced by kind. It is
// two 32 bit halves so the entry is 12 bytes at 4 byte alignment. kind is 1 to 127 in an entry in use.
// kind 0 marks an empty entry, so key_lo is the one 32 bit store that empties an entry and the one that
// fills it. A change of this layout needs a new layout version in main/rtcstate.cpp.
struct entry
{
    uint32_t key_lo; // hash bits 7 to 31, kind in bits 0 to 6
    uint32_t key_hi; // hash bits 32 to 63
    uint16_t w, h;
};
static_assert(sizeof(entry) == 12, "entry layout");

// RTC slow memory, not initialised at boot. Deep sleep, a software reset and a panic restart keep it.
// The magic word of main/rtcstate.cpp tells whether it is valid.
RTC_NOINIT_ATTR entry s_table[SETS][WAYS];
static_assert(sizeof(s_table) == 6144, "table size");

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for(unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

entry* set_of(uint64_t hash)
{
    return s_table[hash & (SETS - 1)];
}

bool in_use(const entry& e)
{
    return (e.key_lo & KIND_MASK) != 0;
}

bool matches(const entry& e, uint64_t hash)
{
    return in_use(e) && e.key_hi == static_cast<uint32_t>(hash >> 32) &&
           (e.key_lo & ~KIND_MASK) == (static_cast<uint32_t>(hash) & ~KIND_MASK);
}
} // namespace

void sizecache_init(bool kept)
{
    if(kept)
    {
        ESP_LOGI(TAG, "table kept in RTC memory");
        return;
    }
    memset(s_table, 0, sizeof(s_table));
    ESP_LOGI(TAG, "table cleared");
}

bool sizecache_get(const std::string& url, int& kind, int& w, int& h)
{
    uint64_t hash = fnv1a(url);
    entry*   set  = set_of(hash);
    for(int i = 0; i < WAYS; i++)
    {
        if(!matches(set[i], hash)) continue;
        kind = set[i].key_lo & KIND_MASK;
        w    = set[i].w;
        h    = set[i].h;
        return true;
    }
    return false;
}

void sizecache_put(const std::string& url, int kind, int w, int h)
{
    if(w <= 0 || h <= 0 || w > 65535 || h > 65535 || kind <= 0 || kind > static_cast<int>(KIND_MASK)) return;
    uint64_t hash = fnv1a(url);
    entry*   set  = set_of(hash);
    entry*   e    = nullptr;
    for(int i = 0; i < WAYS && !e; i++)
        if(matches(set[i], hash)) e = &set[i];
    for(int i = 0; i < WAYS && !e; i++)
        if(!in_use(set[i])) e = &set[i];
    if(!e) e = &set[esp_random() & (WAYS - 1)];
    uint32_t lo = (static_cast<uint32_t>(hash) & ~KIND_MASK) | static_cast<uint32_t>(kind);
    uint32_t hi = static_cast<uint32_t>(hash >> 32);
    if(e->key_lo == lo && e->key_hi == hi && e->w == w && e->h == h) return;
    // A reset between two of these stores leaves the entry empty or complete, never a key with the
    // size of another URL. volatile keeps the stores in this order.
    volatile entry* v = e;
    v->key_lo         = 0;
    v->w              = static_cast<uint16_t>(w);
    v->h              = static_cast<uint16_t>(h);
    v->key_hi         = hi;
    v->key_lo         = lo;
}
