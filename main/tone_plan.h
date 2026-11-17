// -----------------------------------------------------------------------------
// tone_plan: the pulls of one DC balanced draw of tone map entries.
//
// A draw plays two phases. Each phase is a run of pulls on one driver table,
// and at every pull each pixel plays the code of its entry at that pull. A
// code is the plane bit pair (red << 1) | bw: bit 0 goes to the bw plane, bit
// 1 to the red plane.
//   phase 1  EPD_VERB_TABLE_PHASE1, opened at unit 1, EPD_PHASE1_PULL_FRAMES
//            frames a pull. Codes TONE_P1_*: 0 S, 1 I, 2 P, 3 M. The table
//            has no hold code.
//   phase 2  EPD_VERB_TABLE_SEQ, opened at unit TONE_NUDGE_FRAMES,
//            EPD_SEQ_PULL_FRAMES frames a pull. Codes TONE_P2_*: 0 h
//            (ground), 1 p, 2 i, 3 w.
// All pulls of phase 1 play, then all pulls of phase 2. A phase of 0 pulls
// does not play. The letters and their frames are in tonemap.h.
//
// Balance. An entry's net is the sum of its letters' nets, in frames of VDH.
// The draw pays it back on the same pixel: prefix I or P pulls in phase 1,
// EPD_PHASE1_DRIVE frames each, then for the rest one i or p nudge at the
// head of the entry's phase 2 part, TONE_NUDGE_FRAMES frames. Both go in the
// debt's own direction and never overshoot, so a debt of 4 frames is one
// nudge, not a pull and a nudge back, because the drives do not commute.
// What neither pays is the residual. With the letters and TONEMAP_VDHR_PCT of
// tonemap.h every net is a multiple of 4 frames, so every residual is 0. A
// nudge changes the tone, so the table's patterns are written to need none.
//
// Phase 1 order. For an entry, a is its prefix pulls plus its I and P pulls,
// b its S and M pulls. Over the entries a draw counts:
//   B = 1 + max(a)  the prefix and I P part of every entry ends at pull B
//   L = B + max(b)  raised to TONE_SHAKE_MIN when max(a) + max(b) > 0, which
//                   lengthens the S fill and leaves B in place
// Each entry plays S from pull 0 up to B - a, then its prefix and its I and
// P up to B, then S fill up to L - b, then its S and M up to L. B - a is at
// least 1, so every pixel opens the draw with an S pull, the run breaker
// that ends the rail run the previous draw left open. A draw that drives in
// neither phase has L = 0.
//
// Phase 2 order. Its length is the longest nudge plus phase 2 part over the
// entries. Each entry plays h first, then its nudge, then its phase 2
// letters, and ends at the last pull.
//
// Push limit. A pixel is never driven on one rail longer than the stock OTP
// waveform drives it: EPD_PUSH_MAX_VDH, EPD_PUSH_MAX_VDL and
// EPD_PUSH_MAX_VDHR frames in epd97_config.h. A run is the frames on one rail
// until the other rail drives that pixel, ground frames not counted. VDH and
// VDHR drive the same way, so neither ends the other's run, and VDL ends
// both. The positive run, VDH and VDHR frames together until a VDL frame, is
// held to the VDH limit. A pattern is checked with its prefix and nudge, in
// both orders phase 1 can play (with and without S fill). The run left open
// at the end of a draw meets the S pull that opens the next draw. The two VDH
// frames at the head of that pull join an open VDH or positive tail, so a
// pattern whose tail plus that head passes the VDH limit is refused too. A
// table entry that fails any of these is an init error. This is a hardware
// safety rule.
//
// Plane sends. The COG swaps its two plane registers after every pull, so a
// pull without a plane send plays the planes of two pulls back. Codes 0 and
// 3 read the same with the planes swapped. A pull may skip the send only when
// it is not the first pull of its phase and every counted entry's code is 0
// or 3 and equals its code at the pull before. This lets the S pad at the
// head of phase 1 send the planes once.
//
// The module has no device calls and no output. Init expands the table once
// into fixed storage. After that, build and code only read it, so they may
// run on any task.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#include "tonemap.h"

// Pulls of each expanded part of a pattern at most: the I and P part, the S
// and M part, and the phase 2 part.
static const unsigned TONE_SEQ_MAX = 48;

// Phase 1 is at least this many pulls when any counted entry drives in it.
// The S fill takes the rest and erases what the panel showed. Two erase
// tests of 2026-11-13 erased a grid with about 8 pulls of S or M. The prefix
// and the entry's own S and M count toward it.
static const unsigned TONE_SHAKE_MIN = 12;

// Driven frames of a phase 2 p or i pull. Phase 2 opens EPD_VERB_TABLE_SEQ
// at this unit.
static const unsigned TONE_NUDGE_FRAMES = 4;

enum tone_phase
{
    TONE_PHASE_1 = 0,
    TONE_PHASE_2 = 1,
    TONE_PHASE_COUNT = 2
};

// Codes on EPD_VERB_TABLE_PHASE1, the order of EPD_PHASE1_SET_FULL.
enum
{
    TONE_P1_S = 0,
    TONE_P1_I = 1,
    TONE_P1_P = 2,
    TONE_P1_M = 3
};

// Codes on EPD_VERB_TABLE_SEQ.
enum
{
    TONE_P2_H = 0,
    TONE_P2_P = 1,
    TONE_P2_I = 2,
    TONE_P2_W = 3
};

// An expanded pattern. p1a, p1b and p2 are NUL terminated, n1a, n1b and n2
// their lengths.
struct tone_seq
{
    char    p1a[TONE_SEQ_MAX + 1]; // phase 1 I and P before any S or M
    char    p1b[TONE_SEQ_MAX + 1]; // phase 1 S and M
    char    p2[TONE_SEQ_MAX + 1];  // phase 2 letters p, i and w
    uint8_t n1a, n1b, n2;
    int16_t net;      // all letters, frames of VDH
    int8_t  prefix;   // I pulls when above 0, P pulls when below
    int8_t  nudge;    // +1 one i, -1 one p, at the head of phase 2
    // The debt in frames the prefix and the nudge leave unpaid, in the
    // direction that would cancel the net. The entry's net over the whole
    // draw is -residual.
    int8_t  residual;
};

// The push of a pattern in frames, ground frames not counted. vdh, vdl, vdhr
// and pos are the runs left open at the end, max_* the longest runs. pos is
// the positive run.
struct tone_push
{
    unsigned vdh, vdl, vdhr, pos;
    unsigned max_vdh, max_vdl, max_vdhr, max_pos;
};

// The phase lengths of one draw.
struct tone_plan
{
    unsigned len[TONE_PHASE_COUNT]; // pulls of each phase, L and the phase 2 length
    unsigned mid;                   // B: the prefix and I P part of every counted entry ends here
};

// Expands the pattern src into s, with its net, prefix, nudge and residual.
// Returns false, with the reason in why, for a letter outside the notation,
// a repeat count of 0 or over TONE_SEQ_MAX, an S or M after a phase 2
// letter, an S after an M, a part over TONE_SEQ_MAX pulls, a prefix or
// residual outside -127 to 127, a push over the limits, or an open VDH or
// positive tail that the next draw's S head carries over the VDH limit. s is
// not valid after false. why may be null. It is cut to why_len bytes.
bool tone_pattern_expand(const char* src, tone_seq* s, char* why, size_t why_len);

// The push of s as it reaches the panel: per rail the larger of the two
// phase 1 orders, both opened by an S pull, with the prefix and nudge.
void tone_push_of(const tone_seq& s, tone_push* u);

// True when u has a run over EPD_PUSH_MAX_VDH, EPD_PUSH_MAX_VDL or
// EPD_PUSH_MAX_VDHR, or a positive run over EPD_PUSH_MAX_VDH.
bool tone_push_over(const tone_push& u);

// Expands all TONEMAP_COUNT entries once. Returns false with the entry and
// the reason in why when any entry fails. Then no draw may play, and
// tone_plan_build fails. Call it once before the first draw.
bool tone_plan_init(char* why, size_t why_len);

// The expanded pattern of entry e, e below TONEMAP_COUNT, after init.
const tone_seq& tone_plan_seq(unsigned e);

// The plan of one draw. count holds the pixels of each entry in the frame,
// and an entry is counted when its count is not 0. Sets plan->len and
// plan->mid as the file header gives. A frame with no counted entry gets 0
// pulls in both phases. Returns false, with an empty plan, when init has not
// succeeded.
bool tone_plan_build(const uint32_t count[TONEMAP_COUNT], tone_plan* plan);

// Table code of entry e at pull k of phase ph in plan: TONE_P1_* in phase 1,
// TONE_P2_* in phase 2. Valid for an entry the plan counted and k below
// plan.len[ph]. For any other entry the code is not a pull the plan plays,
// so a code table per entry holds 0 for the entries the plan did not count.
uint8_t tone_plan_code(const tone_plan& plan, unsigned e, tone_phase ph, unsigned k);
