// The build of the graph of the tones (main/tone_graph.h) for the generator of main/tone_tables.inc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The graph tables of main/tone_tables.inc, as C++ text appended to inc, from qL, each entry's L in
// q15, and the squeeze parameters k. It prints a summary line. False, with the reason in why, when
// a coloured entry lies in the strip, the panel edge falls below the floor of the squeeze, a region
// has two parts, the geometry does not close, the top of the triangles has a corner that is no tone
// or is not star shaped from the focal point of tone_graph.h, or the graph does not fit its format.
bool tone_graph_build(const int16_t* qL, const int16_t* k, std::string& inc, char* why, size_t len);
