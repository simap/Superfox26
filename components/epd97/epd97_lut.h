// -----------------------------------------------------------------------------
// The two short waveform tables, one per COG revision.
//
// The panel's own waveform lives in its OTP and is what EPD_SLOW runs. These
// replace it, written to register 0xB0 after the 0x44/0x45/0xA7 sequence (which
// is what loads the OTP waveform into the very registers being replaced) and
// before the DC/DC soft start.
//
// The two revisions take different table formats at the same register:
//
//   driver 8   180 bytes = 5 tables x  9 groups x 4 bytes, frame counts
//              packed two to a byte as nibbles
//   driver B   400 bytes = 5 tables x 10 groups x 8 bytes, one whole byte
//              per frame count
//
// A level byte is two bits per phase with phase 0 in the top bits:
// 0 = ground, 1 = VDH, 2 = VDL, 3 = VDHR.
//
// Table index is (red bit << 1) | bw bit, so 0 = paper, 1 = ink, 2 = red,
// 3 = unreachable, 4 = VCOM.
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "epd97_config.h"

#define LUT8_BYTES  180u
#define LUTB_BYTES  400u

// --- driver 8 ----------------------------------------------------------------
// 103 frames at 32.0 ms - 3.3 s - against the panel's own ~51 s.
//
// Group 0 is a balanced shake, group 1 the chopped drive, the remaining seven
// one frame at ground each. Those seven are not optional: a group left at zero
// is not an empty group, it runs about four seconds.
//
// Two findings hold the shape up. The drive has to be chopped, not lengthened:
// short pushes with a rest at ground beat one long run at the rail by a factor
// of four or five on redness while making the ink darker, because a long run
// drags the slower red pigment along behind the black and the rests let it
// relax back. And the shake has to be balanced - as much VDH as VDL, so it adds
// no net charge whatever a pixel holds; without that the ground goes pink
// within a handful of updates.
static const uint8_t k_lut8_base[LUT8_BYTES] = {
  /* table 0, paper (-> white) */
  0x66, 0x33, 0x33, 0x02,
  0x88, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 1, ink (-> black) */
  0x99, 0x33, 0x33, 0x02,
  0x44, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 2, red - overwritten by epd_build_lut8() when the frame has red */
  0x99, 0x33, 0x33, 0x02,
  0x44, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 3, unreachable */
  0x99, 0x33, 0x33, 0x02,
  0x44, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 4, VCOM */
  0x00, 0x33, 0x33, 0x02,
  0x00, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
};

// Build the 180-byte table for this frame.
//
// with_red = the frame carries red, OR the frame before it did. Both matter: a
// frame that FOLLOWS a red one still has to clear the red pigment out or the
// old picture stays visible under the new one.
//
// clearing = there is red to remove but none to make. Every group then goes to
// the erase, because spending them on a red push is wasted and starving the
// erase is what leaves the old picture legible in the white after it.
//
// Red on this film is not a third direction. Both coloured pigments carry the
// same charge and both rise on a positive field; they differ only in threshold,
// red's being the lower one. On the full rail VDH black comes up with the red
// and has to be pulled back out afterwards - and the pull that removes black
// removes red with it, because the two differ in speed by about 2.5, not the
// ten to one the literature suggests. On the reduced rail VDHR black never
// comes up at all, so there is nothing to take back out.
static inline void epd_build_lut8(uint8_t* out, bool with_red, bool clearing)
{
  memcpy(out, k_lut8_base, LUT8_BYTES);
  if (!with_red) {
    return;
  }

  const uint8_t white = out[EPD_8_PAP_TABLE * 9 * 4 + 4];   // 0x88
  const uint8_t black = out[1 * 9 * 4 + 4];                 // 0x44

  // Erase to white, then hold the reduced positive rail. The two swing stages
  // the published three-pigment waveforms carry - activation and separation -
  // both rock on VDH, the rail that brings black up. On a red pixel that is
  // exactly what must not happen, so they are left out and the red table sees
  // only the negative rail and then the reduced positive one.
  const struct { uint8_t level; uint8_t groups; uint8_t rep; } stage[2] = {
    { white,
      clearing ? (uint8_t)EPD_8_RED_CLEAR_GROUPS : (uint8_t)EPD_8_RED_ERASE_GROUPS,
      clearing ? (uint8_t)EPD_8_RED_CLEAR_REP    : (uint8_t)EPD_8_RED_ERASE_REP },
    { (uint8_t)EPD_8_RED_LEVEL,
      clearing ? (uint8_t)0 : (uint8_t)EPD_8_RED_GROUPS,
      (uint8_t)EPD_8_RED_REP },
  };

  // Start at group 2. Group 0 is the shake and group 1 the main drive that
  // makes black black and white white; writing the red stages over group 1
  // replaces nine repeats with one, which is grey blacks and red landing on an
  // undriven pixel.
  unsigned g = 2u;
  for (unsigned st = 0; st < 2u; st++) {
    for (unsigned k = 0; k < stage[st].groups && g < 9u; k++, g++) {
      // Every table gets the timing, not just red's: a group lasts as long as
      // the group says, and a table left at one frame of ground ends it there
      // whatever the others asked for. Ink and paper are held at their own
      // targets throughout - ground is not neutral over stages this long, a
      // black pixel relaxes to grey there and a white one drifts.
      for (unsigned tb = 0; tb < 5u; tb++) {
        uint8_t* q = &out[(tb * 9u + g) * 4u];
        if (tb == (unsigned)EPD_8_RED_TABLE)      q[0] = stage[st].level;
        else if (tb == (unsigned)EPD_8_PAP_TABLE) q[0] = white;
        else if (tb == 4u)                        q[0] = 0x00;   // VCOM
        else                                      q[0] = black;
        // The vendor's own cleaning waveform for this COG works in 480 ms
        // swings, and that is the timescale the pigments respond on. An erase
        // written with the black drive's 32 ms phases does not clear anything.
        q[1] = (uint8_t)EPD_8_RED_FRAMES;
        q[2] = (uint8_t)EPD_8_RED_FRAMES;
        q[3] = stage[st].rep;
      }
    }
  }
}

// --- driver B ----------------------------------------------------------------
// 3460 ms against 33400 ms for the panel's own waveform, measured with a camera
// against a conditioned film: ink at L* 15.1 with a* +1.88, paper at L* 86.9,
// contrast 71.8.
//
// Two things here are not what the 084 does. Tables 0 and 1 take one half of
// the pixels each - driving either alone leaves a clean checkerboard - so they
// get opposite drives rather than the 084's "one white table, three black". And
// the plain shake has to be split: a shake writes the same level into every
// table, which swamps that division and pushes the whole panel one way.
static inline void put_group_b(uint8_t* q, uint8_t level, uint8_t on,
                               uint8_t off, uint8_t rep)
{
  q[0] = level;
  q[1] = on; q[2] = off; q[3] = (uint8_t)EPD_B_PH2;
  q[4] = on; q[5] = off; q[6] = (uint8_t)EPD_B_PH2;
  q[7] = rep;
}

// Clear before setting: a pixel on its way to red may be coming from black, and
// driving it straight at red leaves what was there showing through. One group,
// not three - of the nine red has, every one spent clearing is one not spent
// pulling.
static inline uint8_t red_level_b(unsigned g)
{
  const unsigned rg = (g >= (unsigned)EPD_B_SHAKE_GROUPS)
                        ? (g - (unsigned)EPD_B_SHAKE_GROUPS) : 0u;
  return (rg < (unsigned)EPD_B_RED_WHITE_GROUPS)
           ? (uint8_t)EPD_B_RED_WHITE_LEVEL
           : (uint8_t)EPD_B_RED_LEVEL;
}

static inline void epd_build_lutb(uint8_t* out)
{
  for (unsigned t = 0; t < 5u; t++) {
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];

      if (t == 4u) {                              // VCOM stays at ground
        put_group_b(q, 0x00, 1, 1, 1);
        continue;
      }

      if (g < (unsigned)EPD_B_SHAKE_GROUPS) {
        // The split shake. 0x66 is VDH,VDL,VDH,VDL and 0x99 the reverse, so
        // each half is balanced over the group and cannot accumulate, but at
        // any instant the two are being pushed apart rather than together.
        uint8_t shk = 0x00;
        if (t == (unsigned)EPD_B_INK_TABLE)      shk = (uint8_t)EPD_B_INK_SHAKE;
        else if (t == (unsigned)EPD_B_PAP_TABLE) shk = (uint8_t)EPD_B_PAP_SHAKE;
        else if (t == (unsigned)EPD_B_RED_TABLE) shk = (uint8_t)EPD_B_RED_SHAKE;
        put_group_b(q, shk, EPD_B_SHAKE_F, EPD_B_SHAKE_F, EPD_B_SHAKE_REP);
        continue;
      }

      if (g < (unsigned)(EPD_B_SHAKE_GROUPS + EPD_B_DRIVE_GROUPS)) {
        uint8_t lvl, on = EPD_B_DRIVE_ON, off = EPD_B_DRIVE_OFF;
        if (t == (unsigned)EPD_B_INK_TABLE) {
          lvl = (uint8_t)EPD_B_INK_LEVEL;
        } else if (t == (unsigned)EPD_B_RED_TABLE) {
          lvl = red_level_b(g);
        } else if (t == (unsigned)EPD_B_PAP_TABLE) {
          // The paper keeps its own rest: full duty on both costs paper
          // brightness, 45/1 everywhere dropped it from L* 76.7 to 67.1.
          lvl = (uint8_t)EPD_B_WHITE_LEVEL;
          on  = EPD_B_PAP_ON;
          off = EPD_B_PAP_OFF;
        } else {
          lvl = 0x00;
        }
        put_group_b(q, lvl, on, off, EPD_B_DRIVE_REP);
        continue;
      }

      if (t == (unsigned)EPD_B_RED_TABLE &&
          g < (unsigned)(EPD_B_SHAKE_GROUPS + EPD_B_RED_GROUPS)) {
        // Red keeps driving where black and white have finished. Those groups
        // sit at ground for every table otherwise, so red - the slow pigment -
        // is taking time nothing else is using.
        put_group_b(q, red_level_b(g), EPD_B_DRIVE_ON, EPD_B_DRIVE_OFF,
                    EPD_B_DRIVE_REP);
        continue;
      }

      put_group_b(q, 0x00, 1, 1, 1);
    }
  }
}

// --- driver B verb table -----------------------------------------------------
// One short table set for a calibration session. It is latched once and pulled
// many times, with new planes sent between pulls. Each table is one verb, and
// the plane bit pair of a pixel selects it. `vcom` is a level code (0 ground,
// 1 VDH, 2 VDL, 3 VDHR) that table 4 holds in phase A. Phase A sources:
//
//   0  hold          vcom, so the net field is zero
//   1  push black    VDH
//   2  pull white    VDL
//   3  red step      VDHR with vcom at ground, else ground (net -vcom)
//   4  VCOM          vcom
//
// Group 0 holds the verb. Phase A runs `unit` frames at those levels, phase C
// 1 frame at ground. Groups 1 to 9 run phases A and C for 1 frame at ground.
// Phases B and D have a count of 0 in every group, which the B1 plays as a
// skip: phthallo/supercon runs its change-only table with TB and TD at 0 in
// every group and measures about 2 frames per group. A 0 in TA, TC or a repeat byte is not a skip and is never written
// here. Every table carries the same counts, because the shortest table ends
// the pull for all of them. A pull is unit + 19 frames.
static inline void epd_build_lutb_verb(uint8_t* out, uint8_t unit, uint8_t vcom)
{
  vcom &= 3u;
  const uint8_t level[5] = { vcom, 1u, 2u, (uint8_t)(vcom == 0u ? 3u : 0u), vcom };
  for (unsigned t = 0; t < 5u; t++) {
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];
      q[0] = 0x00;
      q[1] = 1; q[2] = 0; q[3] = 1;
      q[4] = 1; q[5] = 0; q[6] = 1;
      q[7] = 1;
      if (g == 0u) {
        q[0] = (uint8_t)(level[t] << 6);
        q[1] = unit;
      }
    }
  }
}

// --- driver B seq table ------------------------------------------------------
// Four tables for sequence tests, selected by the plane bit pair. The group
// shape is the one yigitabi5444/supercon26-badge-os measured as a hold on the
// B1 (main.c part_table): every group is one frame, phase C only, record
// `level 01 01 00 01 00 01 01`, SRab 0 skips phases A and B and TD 0 skips D.
// Ten groups make a 10 frame pull. A drive table drives groups 0 to unit - 1,
// so unit is 1 to 9 and the step grows with it. Group 9 is always ground.
// Hold and VCOM are ground throughout. This is the phase 2 nudge set of the
// two table draw: red is p then one or two w.
//
//   0  hold    ground
//   1  paper   VDL, every driven frame
//   2  ink     VDH, every driven frame
//   3  w       VDHR for EPD_SEQ_W_FRAMES frames whatever the unit, then ground
//   4  VCOM    ground
//
// The limits are published in epd97.h.
#ifndef EPD_SEQ_UNIT_MAX
#define EPD_SEQ_UNIT_MAX    9
#endif
#ifndef EPD_SEQ_PULL_FRAMES
#define EPD_SEQ_PULL_FRAMES 10
#endif
#ifndef EPD_SEQ_W_FRAMES
#define EPD_SEQ_W_FRAMES    9
#endif
static inline void epd_build_lutb_seq(uint8_t* out, uint8_t unit)
{
  if (unit == 0u) unit = 1u;
  if (unit > EPD_SEQ_UNIT_MAX) unit = EPD_SEQ_UNIT_MAX;
  const unsigned driven = unit;
  for (unsigned t = 0; t < 5u; t++) {
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];
      uint8_t lvl = 0;   // phase C level code, bits 3 and 2 of the level byte
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

// Frames one pull of a seq table plays, 10, or 0 when a record is not the seq
// shape above.
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

// --- driver B phase 1 table --------------------------------------------------
// The table set for the first half of the draw,
// EPD_VERB_TABLE_PHASE1, built by epd_build_lutb_phase1() from the shapes
// below. It is the single table load of the draw and has no hold code. Code
// 0 is S because the planner pads phase 1 with S fill. Codes 0 and 3 read
// the same with the two planes swapped, so the play loop keeps the planes
// across repeated pad pulls.
//
// A pull is EPD_PHASE1_PULL_FRAMES frames in the seq group shape: phase C
// plays one frame in every group, and phase D plays one more in the last
// groups, so 10 groups carry 17 frames.
//
// The frames are slotted: VDH is driven only in frames 1, 2, 5, 6, 9, 10, 15
// and 16, VDL only in frames 3, 4, 7, 8 and 11 to 14, and a code grounds the
// slots it does not use. Two pixels with different codes are then never on
// opposite rails in the same frame, which is the lateral field across a cell
// edge that showed as a light ring around the mono shake tones
// in photos. The stock set 6 mono shake keeps the
// same rule: white's only drive in that group is a VDH pair in phase with
// black's. The last VDH slot follows the last VDL slot so M can end its
// drive on VDH, and frame 17 is ground for every code, so no pixel keeps a
// rail between pulls. Levels per frame, G ground, H VDH, L VDL:
//
//   0  S  shake         HHLLHHLLHHLLGGGGG   6 H, 6 L, last drive L   net 0
//   1  I  ink           HHGGHHGGHHGGGGHHG   8 H                      net +8
//   2  P  paper         GGLLGGLLGGLLLLGGG   8 L                      net -8
//   3  M  mono shake    GGLLGGLLHHLLLLHHG   8 L, 4 H, last drive H   net -4
//   4  VCOM             ground
//
// Net is in frames of VDH. Every table carries the same counts.
#define EPD_PHASE1_SHAPE_S "HHLLHHLLHHLLGGGGG"
#define EPD_PHASE1_SHAPE_I "HHGGHHGGHHGGGGHHG"
#define EPD_PHASE1_SHAPE_P "GGLLGGLLGGLLLLGGG"
#define EPD_PHASE1_SHAPE_M "GGLLGGLLHHLLLLHHG"
#ifndef EPD_PHASE1_PULL_FRAMES
#define EPD_PHASE1_PULL_FRAMES 17
#endif
#ifndef EPD_PHASE1_DRIVE
#define EPD_PHASE1_DRIVE 8
#endif
// epd97.h carries the same two numbers for the planner. A shape that is not
// the pull length would play truncated or read past its end.
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
  // The first 10 - extra groups play one frame, the last `extra` groups two.
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

// Frames one pull of a phase 1 table plays, or 0 when a record is not the
// shape above, the counts differ between tables, or table 4 is not ground.
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
      if ((q[0] & 0xF0u) != 0u) return 0;          // phases A and B are skipped
      if (v[5] == 0u && (q[0] & 3u) != 0u) return 0;   // a skipped D is ground
    }
    frames += 1u + v[5];
  }
  return frames;
}

// Frames one pull of a verb or seq table plays. 0 when a count byte other than
// TB or TD (offsets 2 and 5 in a group) is zero in any table, or when a table's
// counts differ from table 4's. TB and TD at 0 are the skips
// epd_build_lutb_verb() writes.
static inline uint32_t epd_lutb_verb_frames(const uint8_t* lut)
{
  uint32_t frames = 0;
  for (unsigned g = 0; g < 10u; g++) {
    const uint8_t* v = &lut[(4u * 10u + g) * 8u];
    for (unsigned t = 0; t < 5u; t++) {
      const uint8_t* q = &lut[(t * 10u + g) * 8u];
      for (unsigned i = 1; i < 8u; i++) {
        if (q[i] != v[i]) return 0;
        if (q[i] == 0u && i != 2u && i != 5u) return 0;
      }
    }
    frames += ((uint32_t)(v[1] + v[2]) * v[3] + (uint32_t)(v[4] + v[5]) * v[6]) * v[7];
  }
  return frames;
}

// Offset of the first zero count byte in a driver B table, or -1 when there is
// none. Byte 0 of each group is the level byte and is not a count.
static inline int epd_lutb_zero_count_at(const uint8_t* lut)
{
  for (unsigned off = 0; off < LUTB_BYTES; off++) {
    if ((off % 8u) != 0u && lut[off] == 0u) return (int)off;
  }
  return -1;
}

// Frames one pull of a driver B table plays, counted one-based from table 4.
// 0 when any count byte in any table is zero, or when a table's counts differ
// from table 4's. A zero count is not a skip: the group runs for seconds.
static inline uint32_t epd_lutb_uniform_frames(const uint8_t* lut)
{
  uint32_t frames = 0;
  for (unsigned g = 0; g < 10u; g++) {
    const uint8_t* v = &lut[(4u * 10u + g) * 8u];
    for (unsigned t = 0; t < 5u; t++) {
      const uint8_t* q = &lut[(t * 10u + g) * 8u];
      for (unsigned i = 1; i < 8u; i++) {
        if (q[i] == 0u || q[i] != v[i]) return 0;
      }
    }
    frames += ((uint32_t)(v[1] + v[2]) * v[3] + (uint32_t)(v[4] + v[5]) * v[6]) * v[7];
  }
  return frames;
}
