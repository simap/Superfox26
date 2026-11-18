// FreeType faces from the data partition and the glyph cache.

#include "fonts.h"

#include <algorithm>
#include <cstring>
#include "esp_log.h"

#include "config.h"

static const char* TAG = "fonts";

// Under CFG_BASE, by slot. tools/stage_fonts.py copies them from data/fonts, the output of
// tools/fontpack, into the partition image.
static const char* const FACE_FILES[NFACES] = {
    "/fonts/sans.ttf",  "/fonts/sans_b.ttf",  "/fonts/sans_i.ttf",  "/fonts/sans_bi.ttf",
    "/fonts/serif.ttf", "/fonts/serif_b.ttf", "/fonts/serif_i.ttf", "/fonts/serif_bi.ttf",
    "/fonts/mono.ttf",  "/fonts/mono_b.ttf",  "/fonts/mono_i.ttf",  "/fonts/mono_bi.ttf",
    "/fonts/fallback.otf",
};

bool face_set::init()
{
    if(FT_Init_FreeType(&lib))
    {
        ESP_LOGE(TAG, "FT_Init_FreeType failed");
        return false;
    }
    // Every other text face falls back to sans regular, so without it there is no text.
    return get(FACE_SANS) != nullptr;
}

int face_set::resolve(int slot)
{
    if(slot == FACE_FALLBACK) return get(slot) ? slot : -1;
    const int base = slot & ~(FACE_BOLD | FACE_ITALIC), style = slot & (FACE_BOLD | FACE_ITALIC);
    const int chain[] = {slot, base | (style & FACE_BOLD), base | (style & FACE_ITALIC), base, FACE_SANS};
    for(int s : chain)
        if(get(s)) return s;
    return FACE_SANS;
}

FT_Face face_set::get(int slot)
{
    if(tried[slot]) return face[slot];
    tried[slot] = true;
    std::string path = std::string(CFG_BASE) + FACE_FILES[slot];
    FT_Face     f    = nullptr;
    // A stdio stream on the LittleFS file: the TTF stays in flash, FreeType reads tables and glyphs
    // through fseek and fread as it needs them.
    FT_Error err = FT_New_Face(lib, path.c_str(), 0, &f);
    if(err)
    {
        ESP_LOGW(TAG, "%s did not open, FreeType error %d", path.c_str(), err);
        f = nullptr;
    } else
        ESP_LOGI(TAG, "%s: %s %s, %ld glyphs", path.c_str(), f->family_name ? f->family_name : "?",
                 f->style_name ? f->style_name : "", f->num_glyphs);
    face[slot] = f;
    return f;
}

FT_Face face_set::sized(int slot, int px)
{
    FT_Face f = get(slot);
    if(f && cur_px[slot] != px)
    {
        FT_Set_Pixel_Sizes(f, 0, static_cast<FT_UInt>(px));
        cur_px[slot] = px;
        size_switches++;
    }
    return f;
}

void face_set::done()
{
    for(int i = 0; i < NFACES; i++)
        if(face[i]) FT_Done_Face(face[i]);
    if(lib) FT_Done_FreeType(lib);
    lib = nullptr;
}

glyph_cache::entry* glyph_cache::find(uint64_t k)
{
    auto it = idx.find(k);
    if(it == idx.end()) return nullptr;
    lru.splice(lru.begin(), lru, it->second);
    return &*it->second;
}

glyph_cache::entry* glyph_cache::insert(uint64_t k)
{
    lru.emplace_front();
    lru.front().key = k;
    idx[k]          = lru.begin();
    grow(OVERHEAD);
    return &lru.front();
}

void glyph_cache::grow(size_t n)
{
    bytes += n;
    while(bytes > budget && lru.size() > 1)
    {
        entry& e = lru.back();
        bytes -= OVERHEAD + (e.bm ? static_cast<size_t>(e.w) * e.h : 0);
        idx.erase(e.key);
        lru.pop_back();
        evictions++;
    }
    peak = std::max(peak, bytes);
}

void glyph_cache::clear()
{
    idx.clear();
    lru.clear();
    bytes = 0;
}
