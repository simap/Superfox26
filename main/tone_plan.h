// Each entry carries its own frame sequence, so the plan pads each entry to a common length per
// phase and prepends balance frames that cancel most of the entry's net drive, leaving a small
// residual.
// Call tone_plan_init and check that it returns true before tone_plan_build; build returns false
// until init has succeeded.
// tone_plan_init validates tone_tables and fails when an entry's letters disagree with its stored
// net, prefix, nudge or residual, or when a precomputed push run is over the stock push limits.
#pragma once

#include <cstddef>
#include <cstdint>

#include "epd97_config.h"
#include "epd97_lut.h"
#include "tonemap.h"

static const unsigned TONE_SEQ_MAX = 48;

static const unsigned TONE_SHAKE_MIN = 12;

static const unsigned TONE_NUDGE_FRAMES = 4;

enum tone_phase
{
    TONE_PHASE_1 = 0,
    TONE_PHASE_2 = 1,
    TONE_PHASE_COUNT = 2
};

enum
{
    TONE_P1_S = 0,
    TONE_P1_I = 1,
    TONE_P1_P = 2,
    TONE_P1_M = 3
};

enum
{
    TONE_P2_H = 0,
    TONE_P2_P = 1,
    TONE_P2_I = 2,
    TONE_P2_W = 3
};

// An expanded pattern. p2 holds the phase 2 letters h p i w, where h is a hold pull of net 0 that
// counts in n2. split is the count of p2 letters before the pattern's '|', or -1 when it has none.
// tone_plan_code() places phase 2 by it.
struct tone_seq
{
    char    p1a[TONE_SEQ_MAX + 1];
    char    p1b[TONE_SEQ_MAX + 1];
    char    p2[TONE_SEQ_MAX + 1];
    uint8_t n1a, n1b, n2;
    int16_t net;
    int8_t  prefix;
    int8_t  nudge;
    int8_t  residual;
    int8_t  split;
};

struct tone_push
{
    unsigned vdh, vdl, vdhr, pos;
    unsigned max_vdh, max_vdl, max_vdhr, max_pos;
};

struct tone_plan
{
    unsigned len[TONE_PHASE_COUNT];
    unsigned mid;
};

constexpr int tone_p1_net(char c)
{
    switch(c)
    {
    case 'I': return EPD_PHASE1_DRIVE;
    case 'P': return -EPD_PHASE1_DRIVE;
    case 'M': return -4;
    default: return 0;
    }
}

constexpr int tone_p2_net(char c)
{
    switch(c)
    {
    case 'p': return -static_cast<int>(TONE_NUDGE_FRAMES);
    case 'i': return static_cast<int>(TONE_NUDGE_FRAMES);
    case 'w': return static_cast<int>((EPD_SEQ_W_FRAMES * TONEMAP_VDHR_PCT + 50u) / 100u);
    default: return 0;
    }
}

struct tone_balance
{
    int prefix, nudge, residual;
};

constexpr tone_balance tone_balance_of(int net)
{
    const int debt   = -net;
    const int prefix = debt / EPD_PHASE1_DRIVE;
    const int rem    = debt - prefix * EPD_PHASE1_DRIVE;
    const int nudge  = rem / static_cast<int>(TONE_NUDGE_FRAMES);
    return {prefix, nudge, rem - nudge * static_cast<int>(TONE_NUDGE_FRAMES)};
}

constexpr unsigned tone_s_head_vdh()
{
    unsigned n = 0;
    for(const char* f = EPD_PHASE1_SHAPE_S; *f != '\0' && *f != 'L'; f++) n += *f == 'H';
    return n;
}

constexpr bool tone_push_over(const tone_push& u)
{
    return u.max_vdh > EPD_PUSH_MAX_VDH || u.max_vdl > EPD_PUSH_MAX_VDL || u.max_vdhr > EPD_PUSH_MAX_VDHR ||
           u.max_pos > EPD_PUSH_MAX_VDH;
}

// Push limit, a hardware safety rule. A pixel is never driven on one voltage longer than the
// panel's stock waveform drives it: EPD_PUSH_MAX_VDH, EPD_PUSH_MAX_VDL and EPD_PUSH_MAX_VDHR frames
// (epd97_config.h).
//   - A run is the frames on one voltage until the opposite one drives that pixel. Ground frames do
//     not count.
//   - VDH and VDHR drive the same way, so neither ends the other's run. VDL ends both.
//   - The positive run, VDH and VDHR frames together until a VDL frame, is held to the VDH limit.
//   - The run left open at the end of a draw continues into the two VDH frames at the head of the
//     S pull that starts the next draw.
// The generator of main/tone_tables.inc refuses an entry that fails any of these, and
// tone_plan_init() checks the push figures it wrote at boot.
bool tone_plan_init(char* why, size_t why_len);

// The phase 2 length is the longest nudge plus n2 of the entries in use. The hold pad of a split
// pattern is not counted.
bool tone_plan_build(const uint32_t count[TONEMAP_COUNT], tone_plan* plan);

// The code entry e plays at pull k of phase ph. In phase 2 a pattern without a '|' plays its nudge
// and p2 right aligned, so its last letter is on the last pull. A pattern with a '|' plays its nudge
// and the p2 letters before the '|' from pull 0. The letters after the '|' end on the last pull, and
// hold pulls fill the gap between.
uint8_t tone_plan_code(const tone_plan& plan, unsigned e, tone_phase ph, unsigned k);
