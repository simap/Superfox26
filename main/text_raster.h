// Glyph rasterising for text: the FreeType load, an embolden of the outline before it is rendered,
// and the coverage curve applied when a bitmap enters the glyph cache. render.cpp draws text with
// TEXT_RASTER and fallback glyphs with FALLBACK_RASTER. host_tests/tones/text_weight.cpp renders
// glyphs through the same functions with other values and plays them through the tone dither, to
// compare them on the simulated panel.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cmath>
#include <cstdint>

struct text_raster
{
    // FT_Load_Glyph flags, for the advance and the outline. A FT_LOAD_TARGET_* value picks the
    // hinting and the render mode: FT_LOAD_TARGET_MONO renders 1 bit coverage.
    FT_Int32 load;
    // Width and height added to the outline by FT_Outline_EmboldenXY, in 26.6 pixels (64 is one
    // pixel). 0 leaves the outline as hinted. The advance stays as loaded.
    FT_Pos bold_x, bold_y;
    // Coverage curve: coverage c of 255 becomes 255 * (c / 255) ^ (1 / gamma). 1 leaves it, above 1
    // darkens the partly covered edge pixels. 0 and 255 stay as they are.
    float gamma;
    // Rendered coverage at or above solid becomes 255, in place of its value on the curve. 256
    // turns it off.
    int solid;
};

// Text weight of the text faces. Measures and panel images of these and other values:
// host_tests/tones/text_weight.cpp, out/text/report.txt. The values need the owner's eye on the real
// panel.
//
// Hinting. The autohinter at FT_LOAD_TARGET_NORMAL moves vertical stems onto whole pixel columns,
// so a thin stem is one black column. FT_LOAD_DEFAULT runs the fonts' TrueType instructions in the
// version 40 interpreter, which hints heights only: a stem 1 to 1.5 px wide falls across two
// columns of half coverage, two mid grays that the dither breaks into dots. The autohinter also
// changes about a third of the advances by a pixel. FT_LOAD_DEFAULT or FT_LOAD_TARGET_LIGHT gives
// the plain FreeType render.
inline constexpr FT_Int32 TEXT_LOAD = FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;
// Embolden: width added to every outline in 26.6 pixels, 0 to 32 (half a pixel). After the
// autohinter it adds a gray column beside the snapped stems.
inline constexpr FT_Pos TEXT_BOLD = 0;
// Coverage curve exponent, 1 to 2.5. 1 is linear coverage. 1.8 turns coverage 128 into 174 and 64
// into 118, so the partly covered edge pixels darken and full and zero coverage stay.
inline constexpr float TEXT_GAMMA = 1.0f;
// Coverage at or above this draws solid, 160 to 256. 256 is off.
inline constexpr int TEXT_SOLID = 256;

inline constexpr text_raster TEXT_RASTER = {TEXT_LOAD, TEXT_BOLD, 0, TEXT_GAMMA, TEXT_SOLID};

// The fallback face, GNU Unifont: 16 x 16 pixel squares as outlines, drawn at the text's pixel size
// in 1 bit, so each square lands on whole pixels. Antialiased at sizes other than 16 and 32 px the
// squares blur to gray.
inline constexpr text_raster FALLBACK_RASTER = {FT_LOAD_TARGET_MONO, 0, 0, 1.0f, 256};

// The table of the coverage curve and the solid threshold, 256 entries. lut[0] is 0, lut[255] is
// 255, so the cores of stems keep the exact text colour and paper stays paper.
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

// Loads glyph gi for its advance, face->glyph->advance.x in 26.6.
inline FT_Error text_raster_advance(FT_Face face, FT_UInt gi, const text_raster& r)
{
    return FT_Load_Glyph(face, gi, r.load);
}

// Loads glyph gi, emboldens its outline and renders it into face->glyph->bitmap, 8 bit gray or
// 1 bit for FT_LOAD_TARGET_MONO. The curve is not applied here.
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
