// Exposes the bracket tone effect's threshold table and step count.
// The reference model, the host tests and the probe read them to reproduce and check what the
// kernel computes.
// tone_fx_bracket_table() returns the table as int16 values.
// The first value is the starting accumulator.
// The step pairs begin at the third value, one pair per step, each a threshold followed by the
// amount added to the accumulator when the input L is below that threshold.
// tone_fx_bracket_steps() returns the number of step pairs, always a multiple of 8.
#pragma once

#include <cstdint>

const int16_t* tone_fx_bracket_table();

int tone_fx_bracket_steps();
