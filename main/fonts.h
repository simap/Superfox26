// Font faces and glyph cache for the text renderer.
// Opens each TrueType or OpenType face once and keeps a size-bound cache of glyph entries, so the
// renderer can look up a glyph's metrics and bitmap without reopening a font or rasterizing the
// same glyph twice.
// The glyph bitmap in an entry is filled by the renderer, not here.
// Call face_set::init before any face lookup; it fails when the sans face does not open.
// The cache holds at most budget bytes and drops the least recently used entry first, but always
// keeps the most recent one.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

enum
{
    FACE_BOLD       = 1,
    FACE_ITALIC     = 2,
    FACE_SANS       = 0,
    FACE_SERIF      = 4,
    FACE_MONO       = 8,
    FACE_FALLBACK   = 12,
    NFACES          = 13,
};

inline int face_slot(int cls, bool bold, bool italic)
{
    return cls + (bold ? FACE_BOLD : 0) + (italic ? FACE_ITALIC : 0);
}

struct face_set
{
    FT_Library lib            = nullptr;
    FT_Face    face[NFACES]   = {};
    int        cur_px[NFACES] = {};
    bool       tried[NFACES]  = {};

    bool init();
    FT_Face get(int slot);
    int resolve(int slot);
    FT_Face sized(int slot, int px);
};

struct glyph_cache
{
    struct entry
    {
        uint64_t                   key  = 0;
        int32_t                    adv  = 0;
        int16_t                    left = 0, top = 0;
        uint16_t                   w = 0, h = 0;
        uint8_t                    face     = 0;
        bool                       rendered = false;
        std::unique_ptr<uint8_t[]> bm;
    };
    static constexpr size_t OVERHEAD = 8 + sizeof(entry) + 16 + 4;

    std::list<entry>                                         lru;
    std::unordered_map<uint64_t, std::list<entry>::iterator> idx;
    size_t budget;
    size_t bytes = 0, peak = 0;
    long   evictions = 0;

    explicit glyph_cache(size_t budget_bytes) : budget(budget_bytes) {}

    static uint64_t key(int slot, int px, uint32_t c)
    {
        return (static_cast<uint64_t>(slot) << 40) | (static_cast<uint64_t>(px & 0xFFFF) << 21) | (c & 0x1FFFFF);
    }
    entry* find(uint64_t k);
    entry* insert(uint64_t k);
    void   grow(size_t n);
    void   clear();
};
