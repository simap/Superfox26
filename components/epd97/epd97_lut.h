// -----------------------------------------------------------------------------
// The two table sets of a driver B tone session (see the tone section of
// epd97.h): the phase 1 set and the seq set of phase 2.
//
// A tone session replaces the panel's own waveform. The driver writes the set
// to register 0xB0 after the update registers and before the DC/DC soft start.
// The update registers load the OTP waveform into the same registers the set
// overwrites, through their 0x44, 0x45 and 0xA7 steps, so the write order
// matters.
//
// Driver B table format, 400 bytes: 5 tables of 10 groups of 8 bytes. Table t
// drives the pixels whose code is t. Table 4 drives VCOM. The 8 bytes of a
// group:
//   0  level byte, two bits per subphase, A in bits 7-6, then B, C, D in bits
//      1-0. 0 ground, 1 VDH, 2 VDL, 3 VDHR.
//   1  TA, frames of subphase A      4  TC, frames of subphase C
//   2  TB, frames of subphase B      5  TD, frames of subphase D
//   3  SRab, repeats of A then B     6  SRcd, repeats of C then D
//   7  repeats of the group
// A group plays ((TA + TB) * SRab + (TC + TD) * SRcd) * repeats frames. The
// shortest table ends the pull for every table, so every table of a set carries
// the same counts.
//
// Zero bytes. These sets write 0 only where the B1 is known to skip it. A TB or
// TD of 0 skips that subphase. The _steps tables of phthallo/supercon
// (firmware/lib/luts.py) run groups of TA 1, TB 0, TC 1, TD 0 as 2 frames each.
// An SRab of 0 skips subphases A and B. The _fast_table of the same file and
// the calm_table of yigitabi5444/supercon26-badge-os (components/os/epd.c) run
// groups of that shape as one frame. Both projects report that other zeros,
// such as a 0 group repeat, a 0 SRcd, or TA and TB both 0, run a group for
// about 4 s or hang the controller with BUSY low.
//
// These two sets are this project's own. No byte of them comes from the panel's
// OTP or from a vendor document.
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "epd97_config.h"

#define LUTB_BYTES  400u

// --- seq set, phase 2 --------------------------------------------------------
// EPD_TONE_TABLE_SEQ, the table set of phase 2 of a draw. Every group is one
// frame of subphase C only, the record level 01 01 00 01 00 01 01, the one
// frame group named above. Ten groups make a 10 frame pull. A drive table
// drives groups 0 to unit - 1, so unit runs 1 to EPD_SEQ_UNIT_MAX and the step
// grows with it. Group 9 is always ground. VCOM is ground throughout.
//
//   code 0  h  hold    ground
//   code 1  p  paper   VDL, every driven frame
//   code 2  i  ink     VDH, every driven frame
//   code 3  w          VDHR for EPD_SEQ_W_FRAMES frames whatever the unit,
//                      then ground
//   table 4    VCOM    ground
//
// Code 1 is paper here and ink on the phase 1 set.
#define EPD_SEQ_UNIT_MAX     9   // driven frames of a p or i pull, at most
#define EPD_SEQ_PULL_FRAMES 10   // frames of a seq pull, whatever the unit
#define EPD_SEQ_W_FRAMES     9   // VDHR frames of a w pull, whatever the unit
static inline void epd_build_lutb_seq(uint8_t* out, uint8_t unit)
{
  if (unit == 0u) unit = 1u;
  if (unit > EPD_SEQ_UNIT_MAX) unit = EPD_SEQ_UNIT_MAX;
  const unsigned driven = unit;
  for (unsigned t = 0; t < 5u; t++) {
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];
      uint8_t lvl = 0;   // subphase C level, bits 3 and 2 of the level byte
      if (t == 3u) {
        if (g < EPD_SEQ_W_FRAMES) lvl = 3;
      } else if (g < driven) {
        if (t == 1u)      lvl = 2;                           // paper VDL
        else if (t == 2u) lvl = 1;                           // ink VDH
      }
      q[0] = (uint8_t)(lvl << 2);
      q[1] = 1; q[2] = 1; q[3] = 0;
      q[4] = 1; q[5] = 0; q[6] = 1;
      q[7] = 1;
    }
  }
}

static inline uint32_t epd_lutb_seq_frames(const uint8_t* lut)
{
  for (unsigned k = 0; k < 50u; k++) {
    const uint8_t* q = &lut[k * 8u];
    if (q[1] != 1u || q[2] != 1u || q[3] != 0u || q[4] != 1u || q[5] != 0u ||
        q[6] != 1u || q[7] != 1u) {
      return 0;
    }
  }
  return EPD_SEQ_PULL_FRAMES;
}

// --- phase 1 set -------------------------------------------------------------
// EPD_TONE_TABLE_PHASE1, the table set of phase 1 of a draw. It has no hold
// code. Code 0 is S because the planner pads phase 1 with S pulls. Codes 0 and
// 3 read the same with the two planes swapped, so a run of S pulls needs one
// plane send.
//
// A pull is EPD_PHASE1_PULL_FRAMES frames in the seq group shape. Subphase C
// plays one frame in every group, and subphase D one more in the last groups,
// so 10 groups carry 17 frames.
//
// The frames are slotted. VDH is driven only in frames 1, 2, 5, 6, 9, 10, 15
// and 16. VDL is driven only in frames 3, 4, 7, 8 and 11 to 14. A code grounds
// the slots it does not use, so two neighbouring pixels with different codes
// are never on opposite rails in the same frame. A lateral field across a cell
// edge shows as a light ring around the mono shake tones. The stock waveform's
// mono shake keeps the same rule. White's only drive in that group is a VDH
// pair in phase with black's. The last VDH slot follows the last VDL slot so M
// can end its drive on VDH. Frame 17 is ground for every code, so no pixel
// stays on a rail between pulls. Levels per frame, G ground, H VDH, L VDL:
//
//   code 0  S  shake         HHLLHHLLHHLLGGGGG   6 H, 6 L, last drive L   net 0
//   code 1  I  ink           HHGGHHGGHHGGGGHHG   8 H                      net +8
//   code 2  P  paper         GGLLGGLLGGLLLLGGG   8 L                      net -8
//   code 3  M  mono shake    GGLLGGLLHHLLLLHHG   8 L, 4 H, last drive H   net -4
//   table 4    VCOM          ground
//
// Net is in frames of VDH.
#define EPD_PHASE1_SHAPE_S "HHLLHHLLHHLLGGGGG"
#define EPD_PHASE1_SHAPE_I "HHGGHHGGHHGGGGHHG"
#define EPD_PHASE1_SHAPE_P "GGLLGGLLGGLLLLGGG"
#define EPD_PHASE1_SHAPE_M "GGLLGGLLHHLLLLHHG"
#define EPD_PHASE1_PULL_FRAMES 17   // frames of a phase 1 pull
#define EPD_PHASE1_DRIVE        8   // VDH or VDL frames of an I or P pull
// A shape that is not the pull length would play truncated or read past its
// end.
_Static_assert(sizeof(EPD_PHASE1_SHAPE_S) - 1 == EPD_PHASE1_PULL_FRAMES, "S shape is the pull length");
_Static_assert(sizeof(EPD_PHASE1_SHAPE_I) - 1 == EPD_PHASE1_PULL_FRAMES, "I shape is the pull length");
_Static_assert(sizeof(EPD_PHASE1_SHAPE_P) - 1 == EPD_PHASE1_PULL_FRAMES, "P shape is the pull length");
_Static_assert(sizeof(EPD_PHASE1_SHAPE_M) - 1 == EPD_PHASE1_PULL_FRAMES, "M shape is the pull length");
_Static_assert(EPD_PHASE1_PULL_FRAMES >= 10 && EPD_PHASE1_PULL_FRAMES <= 20, "10 groups carry 10 to 20 frames");
static inline void epd_build_lutb_phase1(uint8_t* out)
{
  static const char* const k_shape[4] = {
    EPD_PHASE1_SHAPE_S, EPD_PHASE1_SHAPE_I, EPD_PHASE1_SHAPE_P, EPD_PHASE1_SHAPE_M
  };
  uint8_t lv[4][EPD_PHASE1_PULL_FRAMES];
  for (unsigned t = 0; t < 4u; t++) {
    for (unsigned f = 0; f < (unsigned)EPD_PHASE1_PULL_FRAMES; f++) {
      const char c = k_shape[t][f];
      lv[t][f] = c == 'H' ? 1u : c == 'L' ? 2u : 0u;
    }
  }
  const unsigned extra = (unsigned)EPD_PHASE1_PULL_FRAMES - 10u;
  for (unsigned t = 0; t < 5u; t++) {
    unsigned f = 0;
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];
      const bool two = g >= 10u - extra;
      const uint8_t c = t < 4u ? lv[t][f] : 0u;
      f++;
      uint8_t d = 0;
      if (two) {
        d = t < 4u ? lv[t][f] : 0u;
        f++;
      }
      q[0] = (uint8_t)((c << 2) | d);
      q[1] = 1; q[2] = 1; q[3] = 0;
      q[4] = 1; q[5] = two ? 1u : 0u; q[6] = 1;
      q[7] = 1;
    }
  }
}

static inline uint32_t epd_lutb_phase1_frames(const uint8_t* lut)
{
  uint32_t frames = 0;
  for (unsigned g = 0; g < 10u; g++) {
    const uint8_t* v = &lut[(4u * 10u + g) * 8u];
    if (v[0] != 0u || v[1] != 1u || v[2] != 1u || v[3] != 0u || v[4] != 1u || v[5] > 1u ||
        v[6] != 1u || v[7] != 1u) {
      return 0;
    }
    for (unsigned t = 0; t < 4u; t++) {
      const uint8_t* q = &lut[(t * 10u + g) * 8u];
      for (unsigned i = 1; i < 8u; i++) {
        if (q[i] != v[i]) return 0;
      }
      if ((q[0] & 0xF0u) != 0u) return 0;          // subphases A and B are skipped
      if (v[5] == 0u && (q[0] & 3u) != 0u) return 0;   // a skipped D is ground
    }
    frames += 1u + v[5];
  }
  return frames;
}
