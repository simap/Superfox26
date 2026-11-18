// Remembers across a reset or a wake whether the state other modules kept is still valid and
// whether a reset struck while the panel was drawing.
// A boot uses this to reuse kept state and to halt when a draw was cut short.
// Call rtcstate_begin at boot to learn whether prior state was kept, and call rtcstate_commit once
// per boot so the next boot reads that state as kept.
// Wrap each panel draw with rtcstate_draw_begin and rtcstate_draw_end, and check
// rtcstate_draw_interrupted at boot before starting a new draw.
#pragma once

bool rtcstate_begin();

void rtcstate_commit();

// A panel draw that stops part way leaves the panel with a net DC voltage, and repeated cut draws
// can damage it. After a cut draw the firmware must not draw again until power is removed.
void rtcstate_draw_begin();
void rtcstate_draw_end();
bool rtcstate_draw_interrupted();
