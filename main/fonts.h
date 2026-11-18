// FreeType faces from the data partition and the glyph cache.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

// Face slots. Each class of sans, serif and mono has four styles: slot = class + style bits. The
// fallback face holds the code points the text faces lack (tools/fontpack/fontpack.json).
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

// Thirteen faces, the files of fonts.cpp FACE_FILES on the data partition (config.h). A face opens on
// first use, so a page in one face opens one file. FT_New_Face reads the file as a stream, so the
// outlines stay in flash. Each face keeps one FT_Size, re-sized on a glyph cache miss at another
// pixel size.
struct face_set
{
    FT_Library lib            = nullptr;
    FT_Face    face[NFACES]   = {};
    int        cur_px[NFACES] = {};
    bool       tried[NFACES]  = {};
    long       size_switches  = 0;

    // Initialises FreeType and opens the sans face. False when that file is missing. cfg_init first.
    bool init();
    // Face for slot, or nullptr when its file did not open.
    FT_Face get(int slot);
    // The slot that serves slot: itself when its file opens, else bold italic falls back to bold,
    // then italic, then the class's regular, then sans regular. The fallback face resolves to
    // itself, or to -1 when its file is missing.
    int resolve(int slot);
    // Face for a slot get() opened, with its size object set to px. nullptr when it did not open.
    FT_Face sized(int slot, int px);
    void    done();
};

// Glyph cache keyed by face, pixel size and code point, LRU, capped at a byte budget.
// An entry holds the hinted advance. The coverage bitmap is added on first draw, after the coverage
// curve of text_raster.h.
struct glyph_cache
{
    struct entry
    {
        uint64_t                   key  = 0;
        int32_t                    adv  = 0; // 26.6
        int16_t                    left = 0, top = 0;
        uint16_t                   w = 0, h = 0;
        // The slot whose glyph this is. An entry of a text face with FACE_FALLBACK here holds no glyph:
        // the code point is drawn from the fallback face's entry at the same pixel size.
        uint8_t                    face     = 0;
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

    static uint64_t key(int slot, int px, uint32_t c)
    {
        return (static_cast<uint64_t>(slot) << 40) | (static_cast<uint64_t>(px & 0xFFFF) << 21) | (c & 0x1FFFFF);
    }
    entry* find(uint64_t k);
    entry* insert(uint64_t k);
    void   grow(size_t n);
    void   clear();
};
