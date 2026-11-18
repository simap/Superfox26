// Rasterizes text glyphs through FreeType into pixels for drawing.
// A text_raster groups the glyph load flags, emboldening offsets, gamma curve and solid threshold
// that turn an outline into coverage, so a caller picks one preset for a run of text instead of
// passing each option at every glyph.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cmath>
#include <cstdint>

struct text_raster
{
    FT_Int32 load;
    FT_Pos bold_x, bold_y;
    float gamma;
    int solid;
};

inline constexpr FT_Int32 TEXT_LOAD = FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;
inline constexpr FT_Pos TEXT_BOLD = 0;
inline constexpr float TEXT_GAMMA = 1.0f;
inline constexpr int TEXT_SOLID = 256;

inline constexpr text_raster TEXT_RASTER = {TEXT_LOAD, TEXT_BOLD, 0, TEXT_GAMMA, TEXT_SOLID};

// FT_LOAD_TARGET_MONO makes the fallback face render 1-bit, each glyph drawn at the text pixel
// size.
// The fallback glyphs are squares that must land on whole pixels.
// Antialiasing blurs them to gray at any size other than 16 and 32 px.
inline constexpr text_raster FALLBACK_RASTER = {FT_LOAD_TARGET_MONO, 0, 0, 1.0f, 256};

inline void text_raster_curve(const text_raster& r, uint8_t lut[256])
{
    for(int c = 0; c < 256; c++)
    {
        int v = c;
        if(r.gamma != 1.0f && c > 0 && c < 255)
            v = static_cast<int>(std::lround(255.0 * std::pow(c / 255.0, 1.0 / r.gamma)));
        if(c >= r.solid) v = 255;
        lut[c] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

inline FT_Error text_raster_advance(FT_Face face, FT_UInt gi, const text_raster& r)
{
    return FT_Load_Glyph(face, gi, r.load);
}

inline FT_Error text_raster_render(FT_Face face, FT_UInt gi, const text_raster& r)
{
    FT_Error err = FT_Load_Glyph(face, gi, r.load);
    if(err) return err;
    FT_GlyphSlot g = face->glyph;
    if(g->format == FT_GLYPH_FORMAT_OUTLINE && (r.bold_x || r.bold_y))
        FT_Outline_EmboldenXY(&g->outline, r.bold_x, r.bold_y);
    if(g->format == FT_GLYPH_FORMAT_BITMAP) return 0;
    return FT_Render_Glyph(g, FT_LOAD_TARGET_MODE(r.load) == FT_RENDER_MODE_MONO ? FT_RENDER_MODE_MONO
                                                                                  : FT_RENDER_MODE_NORMAL);
}
