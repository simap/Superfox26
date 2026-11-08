// FreeType faces from the fonts partition and the glyph cache.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

enum { FACE_SANS = 0, FACE_BOLD = 1, FACE_MONO = 2, FACE_MONO_BOLD = 3, NFACES = 4 };

// Four faces: sans regular, sans bold, mono regular, mono bold. Serif maps to sans, italic to upright.
// A missing mono bold falls back to mono, anything else to sans. The TTF files are on the data
// partition (config.h) and opened with FT_New_Face, so they stay in flash. Each face keeps one
// FT_Size, re-sized on a glyph cache miss at another pixel size.
struct face_set
{
    FT_Library lib             = nullptr;
    FT_Face    face[NFACES]    = {};
    int        cur_px[NFACES]  = {};
    bool       tried[NFACES]   = {};
    long       size_switches   = 0;

    // Initialises FreeType and opens the sans face. False when that file is missing. cfg_init first.
    bool init();
    // Face for slot, or nullptr when its file did not open.
    FT_Face get(int slot);
    // Face for slot with its size object set to px. Falls back to mono for mono bold, else to sans.
    FT_Face sized(int slot, int px);
    void    done();
};

// Glyph cache keyed by face, pixel size and code point, LRU, capped at a byte budget.
// An entry holds the hinted advance; the coverage bitmap is added on first draw.
struct glyph_cache
{
    struct entry
    {
        uint64_t                   key  = 0;
        int32_t                    adv  = 0; // 26.6
        int16_t                    left = 0, top = 0;
        uint16_t                   w = 0, h = 0;
        bool                       rendered = false;
        std::unique_ptr<uint8_t[]> bm;
    };
    // bytes counted per entry besides its bitmap: list node + hash node + bucket on a 32-bit target
    static constexpr size_t OVERHEAD = 8 + sizeof(entry) + 16 + 4;

    std::list<entry>                                         lru; // front = most recently used
    std::unordered_map<uint64_t, std::list<entry>::iterator> idx;
    size_t budget;
    size_t bytes = 0, peak = 0;
    long   evictions = 0;

    explicit glyph_cache(size_t budget_bytes) : budget(budget_bytes) {}

    entry* find(uint64_t k);
    entry* insert(uint64_t k);
    void   grow(size_t n);
    void   clear();
};
