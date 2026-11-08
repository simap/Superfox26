#include "sizecache.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include "esp_log.h"
#include "esp_rom_crc.h"

#include "config.h"

static const char* TAG  = "sizecache";
static const char* FILE_PATH = CFG_BASE "/imgsize.bin";
static const int   CAP  = 512;
static const char  MAGIC[4] = {'I', 'S', 'Z', '1'};

namespace
{
struct entry
{
    uint64_t hash;
    uint16_t w, h;
    uint8_t  kind;
    uint8_t  flags;
    uint32_t total; // unused, kept for the file layout
    uint32_t last_used;
};
static_assert(sizeof(entry) == 24, "entry layout");

std::vector<entry> s_entries;
uint32_t           s_tick  = 0; // bumps on every get and put, drives the eviction order
bool               s_dirty = false;

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

entry* find(uint64_t hash)
{
    for(auto& e : s_entries)
        if(e.hash == hash) return &e;
    return nullptr;
}
} // namespace

void sizecache_init()
{
    s_entries.reserve(CAP);
    FILE* f = fopen(FILE_PATH, "rb");
    if(!f) return;
    char     magic[4];
    uint32_t count = 0, crc = 0;
    bool     ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, MAGIC, 4) == 0 && fread(&count, 4, 1, f) == 1 &&
              fread(&crc, 4, 1, f) == 1 && count <= static_cast<uint32_t>(CAP);
    if(ok)
    {
        s_entries.resize(count);
        ok = fread(s_entries.data(), sizeof(entry), count, f) == count;
        if(ok) ok = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(s_entries.data()), count * sizeof(entry)) == crc;
    }
    fclose(f);
    if(!ok)
    {
        ESP_LOGW(TAG, "%s unreadable or corrupt, starting empty", FILE_PATH);
        s_entries.clear();
        return;
    }
    for(auto& e : s_entries) s_tick = std::max(s_tick, e.last_used);
    ESP_LOGI(TAG, "%u image sizes from %s", (unsigned)s_entries.size(), FILE_PATH);
}

bool sizecache_get(const std::string& url, int& kind, int& w, int& h)
{
    entry* e = find(fnv1a(url));
    if(!e) return false;
    e->last_used = ++s_tick;
    kind         = e->kind;
    w            = e->w;
    h            = e->h;
    return true;
}

void sizecache_put(const std::string& url, int kind, int w, int h)
{
    if(w <= 0 || h <= 0 || w > 65535 || h > 65535 || kind <= 0) return;
    uint64_t hash = fnv1a(url);
    entry*   e    = find(hash);
    if(!e)
    {
        if(static_cast<int>(s_entries.size()) >= CAP)
        {
            size_t victim = 0;
            for(size_t i = 1; i < s_entries.size(); i++)
                if(s_entries[i].last_used < s_entries[victim].last_used) victim = i;
            s_entries[victim] = s_entries.back();
            s_entries.pop_back();
        }
        s_entries.push_back(entry{});
        e       = &s_entries.back();
        e->hash = hash;
    } else if(e->kind == kind && e->w == w && e->h == h)
    {
        e->last_used = ++s_tick;
        return;
    }
    e->kind      = static_cast<uint8_t>(kind);
    e->w         = static_cast<uint16_t>(w);
    e->h         = static_cast<uint16_t>(h);
    e->flags     = 0;
    e->total     = 0;
    e->last_used = ++s_tick;
    s_dirty      = true;
}

void sizecache_save()
{
    if(!s_dirty) return;
    FILE* f = fopen(FILE_PATH, "wb");
    if(!f)
    {
        ESP_LOGW(TAG, "cannot write %s", FILE_PATH);
        return;
    }
    uint32_t count = s_entries.size();
    uint32_t crc   = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(s_entries.data()), count * sizeof(entry));
    bool     ok    = fwrite(MAGIC, 1, 4, f) == 4 && fwrite(&count, 4, 1, f) == 1 && fwrite(&crc, 4, 1, f) == 1 &&
              fwrite(s_entries.data(), sizeof(entry), count, f) == count;
    if(fclose(f) != 0) ok = false;
    if(ok) s_dirty = false;
    else ESP_LOGW(TAG, "write of %s failed", FILE_PATH);
}

int sizecache_count()
{
    return static_cast<int>(s_entries.size());
}
