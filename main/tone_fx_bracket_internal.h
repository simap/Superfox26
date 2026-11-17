// The step table of tone_fx_bracket(), for the stand in (host_tests/tone_fx/model) and the tests
// only. The generator of main/tone_tables.inc builds it (host_tests/tone_tables). The kernel is
// described in tone_fx_bracket.cpp.
#pragma once

#include <cstdint>

// The step table: [0] the state S = lo + 256 hi at L 32767, [1] 0, then a threshold T and a delta P
// per step, thresholds falling. A value L takes P of every step with L < T.
const int16_t* tone_fx_bracket_table();

// The number of steps in the table, padded to a multiple of 8 with steps that never fire.
int tone_fx_bracket_steps();
