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

// --- group shape of both sets ------------------------------------------------
// Both sets play a pull of 10 to 20 frames in 10 groups. Every group plays one
// frame of subphase C, the record level 01 01 00 01 00 01 01. The last
// frames - 10 groups play one more frame in subphase D, the same record with
// TD 1. Subphases A and B are skipped in every group.
//
// Packs the levels per frame into a set. Drive table t (0 to 3) plays
// lv[t * frames] to lv[t * frames + frames - 1]. Table 4 (VCOM) is ground.
static inline void epd_lutb_pack(uint8_t* out, const uint8_t* lv, unsigned frames)
{
  const unsigned extra = frames - 10u;
  for (unsigned t = 0; t < 5u; t++) {
    unsigned f = 0;
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];
      const bool two = g >= 10u - extra;
      const uint8_t c = t < 4u ? lv[t * frames + f] : 0u;
      f++;
      uint8_t d = 0;
      if (two) {
        d = t < 4u ? lv[t * frames + f] : 0u;
        f++;
      }
      q[0] = (uint8_t)((c << 2) | d);
      q[1] = 1; q[2] = 1; q[3] = 0;
      q[4] = 1; q[5] = two ? 1u : 0u; q[6] = 1;
      q[7] = 1;
    }
  }
}

// Frames one pull of a set plays, or 0 when a record is not the shape above,
// the counts differ between tables, or table 4 is not ground.
static inline uint32_t epd_lutb_frames(const uint8_t* lut)
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

// --- seq set, phase 2 --------------------------------------------------------
// EPD_TONE_TABLE_SEQ, the table set of phase 2 of a draw. Paper drives in the
// first half of the pull and ink in the second, with a ground frame between,
// so a p pixel beside an i pixel is never on the opposite rail in the same
// frame. Frame 0 and the last frame are ground for every code, so no pixel
// stays on a rail between pulls. A p or i pixel drives the first unit frames
// of its half, so unit runs 1 to EPD_SEQ_UNIT_MAX. Levels per frame at unit 4,
// G ground, L VDL, H VDH, R VDHR:
//
//   code 0  h  hold    GGGGGGGGGGG
//   code 1  p  paper   GLLLLGGGGGG   VDL from frame 1
//   code 2  i  ink     GGGGGGHHHHG   VDH from frame 6
//   code 3  w          GRRRRRRRRRG   VDHR for EPD_SEQ_W_FRAMES frames whatever
//                                    the unit
//   table 4    VCOM    ground
//
// A pixel plays one code per pull, and push runs and net count driven frames
// only, so where the frames fall in the pull does not change them.
//
// Code 1 is paper here and ink on the phase 1 set.
#define EPD_SEQ_UNIT_MAX     4   // driven frames of a p or i pull, at most
#define EPD_SEQ_PULL_FRAMES 11   // frames of a seq pull, whatever the unit
#define EPD_SEQ_W_FRAMES     9   // VDHR frames of a w pull, whatever the unit
_Static_assert(EPD_SEQ_PULL_FRAMES == 2 * (EPD_SEQ_UNIT_MAX + 1) + 1, "two halves and the last ground frame");
_Static_assert(EPD_SEQ_W_FRAMES == EPD_SEQ_PULL_FRAMES - 2, "w drives every frame but the first and the last");
_Static_assert(EPD_SEQ_PULL_FRAMES >= 10 && EPD_SEQ_PULL_FRAMES <= 20, "10 groups carry 10 to 20 frames");
static inline void epd_build_lutb_seq(uint8_t* out, uint8_t unit)
{
  if (unit == 0u) unit = 1u;
  if (unit > EPD_SEQ_UNIT_MAX) unit = EPD_SEQ_UNIT_MAX;
  const unsigned ink_at = EPD_SEQ_UNIT_MAX + 2u;
  uint8_t lv[4][EPD_SEQ_PULL_FRAMES];
  for (unsigned f = 0; f < (unsigned)EPD_SEQ_PULL_FRAMES; f++) {
    lv[0][f] = 0;
    lv[1][f] = f >= 1u && f < 1u + unit ? 2u : 0u;              // paper VDL
    lv[2][f] = f >= ink_at && f < ink_at + unit ? 1u : 0u;      // ink VDH
    lv[3][f] = f >= 1u && f <= EPD_SEQ_W_FRAMES ? 3u : 0u;      // w VDHR
  }
  epd_lutb_pack(out, &lv[0][0], EPD_SEQ_PULL_FRAMES);
}

// --- phase 1 set -------------------------------------------------------------
// EPD_TONE_TABLE_PHASE1, the table set of phase 1 of a draw. It has no hold
// code. Code 0 is S because the planner pads phase 1 with S pulls. Codes 0 and
// 3 read the same with the two planes swapped, so a run of S pulls needs one
// plane send.
//
// A pull is EPD_PHASE1_PULL_FRAMES frames in the group shape above, so the
// last 7 groups play two frames.
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
  epd_lutb_pack(out, &lv[0][0], EPD_PHASE1_PULL_FRAMES);
}
