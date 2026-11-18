// The table is a set-associative cache: the low bits of a URL's hash pick a set, and a set holds
// WAYS entries.
// The low KIND_BITS of key_lo hold the kind, and a nonzero kind marks the entry as in use, so a
// stored kind is never zero.
// An entry keeps the hash with those low bits replaced by the kind, because the set position
// already fixes them.
// sizecache_put clears key_lo first and writes it last, after w, h and key_hi, so a reset partway
// through a write never leaves an entry that reads as valid with wrong fields.

#include "sizecache.h"

#include <cstdint>
#include <cstring>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"

#include "util.h"

static const char*    TAG       = "sizecache";
static const int      SETS      = 128;
static const int      WAYS      = 4;
static const int      KIND_BITS = 7;
static const uint32_t KIND_MASK = (1u << KIND_BITS) - 1;

static_assert((SETS & (SETS - 1)) == 0 && (WAYS & (WAYS - 1)) == 0, "SETS and WAYS are powers of 2");
static_assert(SETS == 1 << KIND_BITS, "kind takes the place of the set index bits in an entry");

namespace
{
struct entry
{
    uint32_t key_lo;
    uint32_t key_hi;
    uint16_t w, h;
};
static_assert(sizeof(entry) == 12, "entry layout");

RTC_NOINIT_ATTR entry s_table[SETS][WAYS];
static_assert(sizeof(s_table) == 6144, "table size");

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
}

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
    uint64_t hash = fnv1a64(url);
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
    uint64_t hash = fnv1a64(url);
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
    volatile entry* v = e;
    v->key_lo         = 0;
    v->w              = static_cast<uint16_t>(w);
    v->h              = static_cast<uint16_t>(h);
    v->key_hi         = hi;
    v->key_lo         = lo;
}
