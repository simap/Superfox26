// -----------------------------------------------------------------------------
// tonemap - seq pattern per tone, for cal draw.
//
// cal draw maps every image pixel to the nearest entry and plays that
// entry's pattern, one character per pull, all pixels in lockstep. A pattern
// is h, p, i and r characters, at most CAL_SEQ_MAX of them. An empty pattern
// holds throughout. cal draw stores the entry index per pixel in one byte, so
// the table has at most 255 entries.
//
// Every entry carries two descriptions of its tone, both from the same
// measurement: a photo of cal grid5 at unit 4, where every tone sits
// between its own black, white and red reference cells, and each tone is
// mapped per channel so that its white neighbour reads 255 and its black
// neighbour reads 25, 27, 41. That takes the light out of the photo: two
// grid 5 photos under different light agreed to a mean of 4.4 RGB units
// over 144 tones. Patterns within an Oklab distance of 0.012 of a shorter
// pattern are left out.
//
// ink, red: the tools/img_to_tone.py pair, ink = 255 - max(R,G,B) and
// red = max(0, R - max(G,B)), from the normalized RGB. The default mapping
// (show tri, show n) works in this pair. In it the panel's black is ink
// TONEMAP_BLACK_INK with no red and its red corner is ink TONEMAP_RED_INK,
// red TONEMAP_RED_RED. An image pixel uses the full 0 to 255 scale, so cal
// draw maps the image triangle (white, black, red) onto the panel triangle
// before the nearest entry search. Image coordinates obey ink + red <= 255.
// The steps, all integer in draw_map():
//   sat   = min(255, red * 255 / (255 - ink) * 255 / TONEMAP_SAT_FULL)
//   ink_e = ink - ink * sat * TONEMAP_RED_LIFT / 255 / 255
//   ink_e = min(255, ink_e * 255 / TONEMAP_INK_FULL)
//   red_e = sat * (255 - ink_e) / 255
//   ink'  = (ink_e * TONEMAP_BLACK_INK + red_e * TONEMAP_RED_INK) / 255
//   red'  = red_e * TONEMAP_RED_RED / 255
// The nearest entry is by ink distance squared plus TONEMAP_RED_WEIGHT times
// red distance, squared.
//
// R, G, B: the normalized photo color. The CIE mapping, the default for show
// and draw, converts these to Oklab at run time, takes the paper,
// iiiii and red corner entries as the triangle's corners in Oklab, places
// each pixel as the barycentric mix of those three, and picks the nearest
// entry by Oklab distance.
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

typedef struct {
  uint8_t     ink;
  uint8_t     red;
  uint8_t     r, g, b;
  const char *pattern;
} tone_entry_t;

#define TONEMAP_BLACK_INK 214   // iiiii
// The red corner is the entry the eye reads as the deepest red on the color
// triangle, rrrrrrirrrrrr.
#define TONEMAP_RED_INK   101
#define TONEMAP_RED_RED   113
#define TONEMAP_RED_PATTERN "rrrrrrirrrrrr"   // the red corner, both mappings
// At 255, 255, 0 and 1 the knee, the lift and the weight are off and the
// mapping is the plain triangle map with nearest entry.
#define TONEMAP_INK_FULL   255  // image ink that reaches the panel's black
#define TONEMAP_SAT_FULL   255  // image saturation, of 255, that counts as full red
#define TONEMAP_RED_LIFT     0  // ink a fully saturated pixel gives up, of 255
#define TONEMAP_RED_WEIGHT   1  // red distance weight in the nearest entry search
// An image pixel with red below this is gray and matches only neutral
// entries. The white to black ramp then never shows a maroon, whatever the
// gray series lacks. 1 means exactly red 0 only. Both mappings apply it.
#define TONEMAP_GRAY_MAX_RED 8
// Neutral in the pair mapping is red 0. The camera red proxy misses faint
// pinks (irrrr, rirrr, rrri read red 0 with Oklab a +0.02), so the CIE
// mapping calls an entry neutral by its Oklab a instead, at or below this.
#define TONEMAP_GRAY_A_MAX 0.010f
// The CIE red corner: a fully saturated image pixel targets this entry, so
// it caps the brightness and chroma of every red. rrrrrrrrrrrrr is the most
// chromatic entry measured, L 0.52 C 0.19. TONEMAP_RED_PATTERN, L 0.45 C
// 0.16, is the darker red the pair mapping and the marks use.
#define TONEMAP_CIE_RED_PATTERN "rrrrrrrrrrrrr"
// The CIE path raises the ink weight to this power, of 1.0, before the mix.
// Above 1 lifts the midtones: at 1.4 ink 128 becomes 97. The panel paper is
// about L* 77 and its black 0.23 of paper, so a linear L map reads dark.
#define TONEMAP_GAMMA 0.9f
// The CIE nearest entry search multiplies the a and b differences by this
// before squaring. Above 1 a hue or chroma miss costs more than the same
// lightness miss, so a near gray pixel takes a gray a step off in lightness
// over a pink at the right lightness.
#define TONEMAP_CIE_CHROMA_WEIGHT 3.0f

// Calibration marks: squares of this size in the four corners of every grid
// and draw, white, black, deepest red and mid gray, for photo normalization.
// 0 for none.
#define TONEMAP_MARK_PX     56

// show <n> ref: the tone image as the stock three colors. A pixel is red at
// red >= TONEMAP_REF_RED, else black at ink >= TONEMAP_REF_INK, else white.
#define TONEMAP_REF_RED     96
#define TONEMAP_REF_INK    128

// Grid 5 with white cells, 2026-11-08, sorted by Oklab lightness. The
// comment holds Oklab L and a. Generated by the session scratchpad
// grid/maptable.py from grid/measure5.py output.
static const tone_entry_t k_tonemap[] = {
  {   0,   0, 255, 255, 255, ""              },   // paper
  {   0,   6, 255, 248, 249, "irrrrrrpp"       },   // L 0.98 a +0.01
  {   0,  12, 255, 235, 243, "rrrrprrrrprrrrp" },   // L 0.96 a +0.02
  {   0,  26, 255, 220, 229, "rrrrrrrrrp"      },   // L 0.93 a +0.04
  {  21,   0, 223, 228, 234, "ipip"            },   // L 0.92 a -0.00
  {   0,  32, 255, 212, 223, "rrrrrrrrrrrrp"   },   // L 0.91 a +0.05
  {  17,  15, 238, 216, 223, "irrrrrrp"        },   // L 0.90 a +0.03
  {  29,   0, 220, 221, 226, "rrpr"            },   // L 0.90 a +0.00
  {   9,  26, 246, 206, 220, "irrrrrrrrp"      },   // L 0.89 a +0.05
  {   2,  39, 253, 201, 214, "irrrrrrrrrrrp"   },   // L 0.89 a +0.06
  {  43,   0, 207, 200, 212, "rrprr"           },   // L 0.84 a +0.01
  {  46,   0, 199, 199, 209, "prrr"            },   // L 0.83 a +0.00
  {  49,   0, 206, 192, 206, "rrrprr"          },   // L 0.82 a +0.02
  {  31,  24, 224, 179, 200, "rrrrrrrrrpprr"   },   // L 0.81 a +0.06
  {  51,   0, 190, 193, 204, "iip"             },   // L 0.81 a +0.00
  {  29,  45, 226, 159, 181, "rrrrrrrrrpr"     },   // L 0.77 a +0.08
  {  64,   5, 191, 170, 186, "rrrprrr"         },   // L 0.76 a +0.03
  {  67,   0, 171, 175, 188, "ipiip"           },   // L 0.75 a +0.00
  {  72,   2, 183, 165, 181, "prrrrr"          },   // L 0.74 a +0.03
  {  68,  20, 187, 147, 167, "rrrrprrr"        },   // L 0.71 a +0.05
  {  80,   3, 175, 152, 172, "rrpprrrrr"       },   // L 0.71 a +0.03
  {  84,   6, 171, 148, 165, "pprrrrrr"        },   // L 0.69 a +0.03
  {  77,  16, 178, 145, 162, "rrrrr"           },   // L 0.69 a +0.04
  {  49,  66, 206, 122, 140, "rrrrprrrrprrrr"  },   // L 0.67 a +0.11
  {  87,  15, 168, 137, 153, "prrrrrr"         },   // L 0.66 a +0.04
  {  50,  68, 205, 116, 137, "rrrrrrrrrprr"    },   // L 0.66 a +0.11
  {  74,  33, 181, 128, 148, "rrprrrrr"        },   // L 0.66 a +0.07
  {  47,  78, 208, 111, 130, "rrrrrrrrrpprrrrr" },  // L 0.66 a +0.12
  {  65,  59, 190, 112, 131, "rrrrprrrrr"      },   // L 0.64 a +0.10
  {  89,  26, 166, 122, 140, "pprrrrrrr"       },   // L 0.63 a +0.06
  {  75,  52, 180, 110, 128, "rrrpprrrrrrr"    },   // L 0.62 a +0.09
  { 106,   0, 121, 135, 149, "pi"              },   // L 0.62 a -0.01
  { 111,   0, 126, 125, 144, "iiip"            },   // L 0.60 a +0.01
  {  83,  56, 172,  98, 116, "rrrrrrr"         },   // L 0.58 a +0.10
  {  52, 132, 203,  56,  71, "irrrrrrrrprrrrrr" },  // L 0.57 a +0.17
  {  75,  82, 180,  80,  98, "rrrprrrrrrr"     },   // L 0.56 a +0.13
  {  82,  71, 173,  86, 102, "rrrrrrrr"        },   // L 0.56 a +0.11
  {  69,  98, 186,  72,  88, "rrrpprrrrrrrrr"  },   // L 0.56 a +0.14
  {  78,  89, 177,  69,  88, "rrrrrrrrr"       },   // L 0.54 a +0.14
  {  72, 108, 183,  61,  75, "rrrrrrrrrr"      },   // L 0.54 a +0.15
  { 128,   0, 100, 109, 127, "i"               },   // L 0.53 a -0.00
  { 135,   0, 117, 103, 120, "irrrr"           },   // L 0.53 a +0.02
  {  68, 125, 187,  50,  62, "prrrrrrrrrrrr"   },   // L 0.53 a +0.16
  {  67, 139, 188,  36,  49, "rrrrrrrrrrrrr"   },   // L 0.52 a +0.17
  { 137,   0, 105, 100, 118, "iiiiip"          },   // L 0.51 a +0.01
  {  76, 118, 179,  46,  61, "rrrrrrrrrrr"     },   // L 0.51 a +0.16
  { 118,  38, 137,  85,  99, "irrrrrr"         },   // L 0.51 a +0.07
  {  77, 127, 178,  42,  51, "irrrrrrrrrrr"    },   // L 0.51 a +0.16
  { 145,   0, 107,  93, 110, "rirrr"           },   // L 0.50 a +0.02
  {  76, 135, 179,  26,  44, "rrrrrrprrrrrrrrr" },  // L 0.49 a +0.17
  {  83, 123, 172,  39,  49, "irrrrrrrrrrrrrr" },   // L 0.49 a +0.16
  {  89, 105, 166,  47,  61, "rrrrpirrrrrrr"   },   // L 0.49 a +0.15
  {  96,  92, 159,  56,  67, "rrrrrrpirrrr"    },   // L 0.49 a +0.13
  { 115,  53, 140,  71,  87, "rrrrpirrrr"      },   // L 0.49 a +0.09
  { 105,  79, 150,  56,  71, "rrrpirrrrrr"     },   // L 0.48 a +0.12
  {  96, 111, 159,  41,  48, "rrrirrrrrrr"     },   // L 0.47 a +0.14
  { 102,  94, 153,  48,  59, "rirrrrrrrr"      },   // L 0.47 a +0.13
  { 152,   0,  86,  86, 103, "iiiip"           },   // L 0.46 a +0.01
  { 112,  76, 143,  52,  67, "rrpirrrrrrr"     },   // L 0.46 a +0.12
  { 153,   0,  78,  87, 102, "ipi"             },   // L 0.45 a -0.00
  { 101, 113, 154,  32,  41, "rrrrrrirrrrrr"   },   // L 0.45 a +0.14, the red corner
  { 118,  71, 137,  53,  66, "rrrrrrpirrr"     },   // L 0.45 a +0.11
  { 123,  77, 132,  49,  55, "rrrrrrrrrrrri"   },   // L 0.43 a +0.11
  { 115,  95, 140,  33,  45, "rrrrrirrrrr"     },   // L 0.43 a +0.13, the red corner mark
  { 143,  36, 112,  60,  76, "rrirrrr"         },   // L 0.43 a +0.08
  { 173,   0,  82,  69,  82, "rrri"            },   // L 0.41 a +0.02
  { 130,  89, 125,  27,  36, "rrrrrrrrrrrrir"  },   // L 0.39 a +0.12
  { 171,   0,  61,  67,  84, "ipipi"           },   // L 0.38 a -0.00
  { 133,  93, 122,  17,  29, "rrrrrrrrrirrr"   },   // L 0.38 a +0.13
  { 153,  49, 102,  43,  53, "rrrrrrrrrii"     },   // L 0.37 a +0.08
  { 177,   0,  69,  60,  78, "iirrrr"          },   // L 0.37 a +0.02
  { 163,  36,  92,  49,  56, "rrrrirrrrirrrr"  },   // L 0.37 a +0.06
  { 164,  32,  91,  45,  59, "iirrrrrr"        },   // L 0.36 a +0.07
  { 153,  61, 102,  29,  41, "rrrrrrrrri"      },   // L 0.35 a +0.10
  { 160,  52,  95,  32,  43, "irrrrrrrri"      },   // L 0.34 a +0.09
  { 179,  14,  76,  45,  62, "rrrrri"          },   // L 0.34 a +0.05
  { 187,   0,  55,  50,  68, "iirrr"           },   // L 0.33 a +0.01
  { 169,  40,  86,  33,  46, "irrrrrrri"       },   // L 0.33 a +0.08
  { 152,  81, 103,   6,  22, "rrrrrrrrrir"     },   // L 0.33 a +0.12
  { 175,  39,  80,  30,  41, "rrrrrrri"        },   // L 0.31 a +0.07
  { 180,  29,  75,  33,  46, "irrrrrri"        },   // L 0.31 a +0.07
  { 192,   0,  42,  46,  63, "ii"              },   // L 0.30 a +0.00
  { 184,  28,  71,  30,  43, "rrrirrrirrrirrr" },   // L 0.30 a +0.06
  { 180,  36,  75,  26,  39, "irrrrrrirr"      },   // L 0.30 a +0.08
  { 201,   0,  39,  42,  54, "ipii"            },   // L 0.29 a +0.00
  { 204,   0,  43,  38,  51, "iiirrrrrrrr"     },   // L 0.28 a +0.01
  { 205,   2,  50,  35,  48, "rrirrirrirrirr"  },   // L 0.28 a +0.03
  { 194,  21,  61,  27,  40, "irrrrrriiii"     },   // L 0.28 a +0.06
  { 199,   0,  42,  35,  56, "irirr"           },   // L 0.27 a +0.02
  { 206,   0,  35,  33,  49, "iipii"           },   // L 0.26 a +0.01
  { 204,   0,  25,  33,  51, "iii"             },   // L 0.25 a -0.00
  { 210,   0,  32,  30,  45, "iririrr"         },   // L 0.24 a +0.01
  { 207,  13,  48,  20,  35, "irrrrrrii"       },   // L 0.24 a +0.05
  { 214,   0,  25,  27,  41, "iiiii"           },   // L 0.23 a +0.00, black
  { 214,   0,  20,  24,  41, "iiii"            },   // L 0.21 a +0.00
  { 221,   0,  16,  17,  34, "iiirrrrrr"       },   // L 0.19 a +0.01
  { 223,  11,  32,   3,  21, "irrrrrriii"      },   // L 0.17 a +0.06
};

#define TONEMAP_COUNT (sizeof(k_tonemap) / sizeof(k_tonemap[0]))

// Candidates: patterns cal grid4 draws after the map entries so a photo can
// measure them, and nothing else uses. None at the moment. cal grid5 draws
// every pattern of the grids and the map, so new candidates go into
// k_grid3 or here.
#define TONEMAP_CANDIDATES 0u
static const char *const k_tonemap_candidates[1] = { "" };
