// -----------------------------------------------------------------------------
// tonemap: the tones the panel shows, one entry per tone.
//
// An entry is one measured tone and the pulls that make it:
//   L, a, b  the tone in Oklab, on the scale where the black reference reads
//            L 0 and the paper reference L 1. The table holds Oklab and not
//            sRGB counts, so an entry is not rounded to 8 bits and not held
//            to the sRGB gamut.
//   pattern  the pulls every pixel of the tone plays, in the notation below.
//            tone_plan.h expands it and adds the pulls that make each draw
//            DC balanced.
// The tables the tone code reads are built from this table on the host, into
// main/tone_tables.inc. After a change here, regenerate that file
// (host_tests/tone_tables, make generate). The firmware does not build while
// it is stale.
// Entry 0 is the paper. A frame stores the entry index per pixel in one byte.
//
// The table holds 103 entries: the 101 tones of the lists c97a and c97b of
// the combo test page, with the paper and the black reference as the two
// ends. All were measured on that page. A tone on
// both lists is the mean of the two. Each reading is the tone's share of the
// light from the photo's black to its paper, per channel, so black is sRGB 0
// 0 0 and paper 255 255 255. That is a picture for a monitor, which is what
// an image is too. A painting of a test image from these colours matched the
// photo of the panel. A
// pattern with no r or R is black and white only, so its a and b are 0. The
// entries are sorted by lightness.
//
// Pattern notation. Phase 1 letters, one pull each on the phase 1 table, net
// in frames of VDH:
//   I  ink         8 VDH                 net +8
//   P  paper       8 VDL                 net -8
//   S  shake       6 VDH and 6 VDL       net 0
//   M  mono shake  8 VDL and 4 VDH       net -4
// Phase 2 letters, on the seq table:
//   i  ink nudge, one pull of 4 VDH, net +4
//   p  paper nudge, one pull of 4 VDL, net -4
//   r  p then one w pull, R is p then two w pulls. A w pull drives 9 VDHR
//      frames, net TONEMAP_VDHR_PCT percent of them.
// A digit run repeats the letter before it. The I and P before any S or M
// play first in phase 1, then the S and M. An I or P after an S or M plays in
// phase 2 as i i or p p. Nothing but M follows M, and no S or M follows a
// phase 2 letter. A '|' or an 'h' is a placement mark of the measurement page.
// It adds no pull and is skipped, with any digit run after it. Each of the I
// and P part, the S and M part and the phase 2 part is at most TONE_SEQ_MAX
// pulls after expansion (tone_plan.h).
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

// VDHR to VDH ratio in percent, for the net of a w pull: 4 frames of VDH per
// 9 of VDHR. At 44 the stock red table of OTP set 6 is net zero, 220 VDH, 310
// VDL and 204 VDHR.
#define TONEMAP_VDHR_PCT 44

// The paper and black references, entry 0 and the last entry. From the hunts
// of 2026-11-11: black read at the stock cell within 0.007 Oklab over nine
// copies, paper within 0.009 over six.
#define TONEMAP_PAPER_PATTERN "SM3p15"
#define TONEMAP_BLACK_PATTERN "SM3i15"

// The table says what the panel shows, and the panel's reds are darker and
// duller than an image's. The mapping brings an image colour into the
// panel's range before it takes the nearest entry, see tonemap_squeeze.h.
// Chroma is always scaled, image edge to panel edge. This sets how far the
// lightness of a saturated colour moves toward the panel's strongest red.
// 0: lightness kept, a bright red draws as a pink. 1: a bright red draws as
// the strongest red, dark. Below 0: no squeeze, the nearest entry only, which
// clips.
#define TONEMAP_COMPRESS 0.75f

// The tone curve, on the colour's Oklab lightness: L to the power 1 / gamma,
// and the chroma keeps its share of the image's range. 1.0 changes nothing,
// above 1 draws lighter, under 1 darker.
#define TONEMAP_GAMMA 1.0f

// An entry is neutral when its Oklab a is at or below this. A gray pixel
// takes neutral entries only, so the white to black ramp never shows a pink.
#define TONEMAP_GRAY_A_MAX 0.001f

// The nearest entry search multiplies the a and b differences by this before
// squaring. Above 1 a hue or chroma miss costs more than the same lightness
// miss, so a near gray pixel takes a gray a step off in lightness over a pink
// at the right lightness.
#define TONEMAP_CIE_CHROMA_WEIGHT 2.0f

struct tone_entry
{
    float       L, a, b;
    const char* pattern;
};

// constexpr, so the build checks main/tone_tables.inc against it
// (tone_tables_source.h). A unit that reads it at run time gets its own copy.
static constexpr tone_entry k_tonemap[] = {
#include "tonemap_entries.inc"
};

#define TONEMAP_COUNT (sizeof(k_tonemap) / sizeof(k_tonemap[0]))

static_assert(TONEMAP_COUNT <= 256, "a frame holds the entry index in one byte");
