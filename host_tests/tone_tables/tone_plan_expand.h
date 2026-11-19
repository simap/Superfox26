// Pattern expansion of main/tone_plan.h, for the generator of main/tone_tables.inc and the host
// tests. The firmware reads the expansions from that file and does not build this code.
#pragma once

#include <cstddef>

#include "tone_plan.h"

// Expands the pattern src into s, with its net, prefix, nudge and residual.
// Returns false, with the reason in why, for a letter outside the notation,
// a repeat count of 0 or over TONE_SEQ_MAX, more than one '|', a repeat
// count after a '|', an S or M after a phase 2 letter or a '|', an S after
// an M, a part over TONE_SEQ_MAX pulls, a prefix or
// residual outside -127 to 127, a push over the limits, or an open VDH or
// positive tail that the next draw's S head carries over the VDH limit. s is
// not valid after false. why may be null. It is cut to why_len bytes.
bool tone_pattern_expand(const char* src, tone_seq* s, char* why, size_t why_len);

// The push of s as it reaches the panel: per rail the larger of the two
// phase 1 orders, both opened by an S pull, with the prefix and nudge.
void tone_push_of(const tone_seq& s, tone_push* u);
