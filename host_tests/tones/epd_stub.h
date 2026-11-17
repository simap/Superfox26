// Host model of the epd97 calls tones_draw() makes. It plays no panel. It records each session and
// pull and checks the code every pixel plays against tone_plan_code() for that pixel's entry.
#pragma once

#include <cstddef>
#include <cstdint>

#include "tone_plan.h"

struct epd_stub_log
{
    // in
    bool            no_tables = false;   // epd_verb_open_table() returns ESP_ERR_NOT_SUPPORTED
    const uint8_t*  frame     = nullptr; // the frame tones_draw() plays, TONE_W x TONE_H
    tone_plan       plan      = {};      // its plan
    const uint32_t* count     = nullptr; // its entry counts
    // out
    unsigned opens = 0, closes = 0, clears = 0, refreshes = 0;
    unsigned pulls[TONE_PHASE_COUNT] = {0, 0}; // pulls played per phase
    unsigned sends       = 0;                  // pulls with a plane send
    unsigned first_kept  = 0;                  // first pulls of a session without a plane send
    unsigned bad_open    = 0;                  // opens on a wrong table or unit, or out of phase order
    size_t   mismatches  = 0;                  // pixel pulls whose code differs from tone_plan_code()
    int      refresh_mode = -1;                // the last refresh_display() mode
};

extern epd_stub_log g_stub;
