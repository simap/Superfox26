// The tables of the tone code, built on the host from the tone table into main/tone_tables.inc
// (host_tests/tone_tables). The device computes none of them.
//
// Everything in namespace tone_tables is constexpr. A unit that needs a table in RAM declares its
// own variable initialized from it, with TONE_TABLES_RAM, and the start up code copies that from
// flash. A unit that reads a table from flash uses it here. The owners and the placement:
//   squeeze_k          RAM, tone_fx_squeeze.cpp, per chunk
//   bracket, neutral   RAM, tone_fx_bracket.cpp, per chunk. The neutral list stays in flash.
//   search, search_max RAM, tone_fx_search.cpp, per coloured pixel
//   qL, q2a, q2b, qlin RAM, tones.cpp, per pixel
//   noise_blue         RAM, tones.cpp, per pixel
//   white              flash, copied into the chunk's slot WHITE by tones_init()
//   solid_gray         flash, tones_solid_gray(), per tab bar fill
//   seq, push          flash, tone_plan.cpp, per draw and at boot
#pragma once

#include <array>
#include <cstdint>

#include "tone_fx.h"
#include "tone_fx_bracket.h"
#include "tone_fx_search_internal.h"
#include "tone_fx_squeeze_internal.h"
#include "tone_plan.h"
#include "tone_tables_source.h"
#include "tonemap.h"

#include "tone_tables.inc"

// The placement of a table in internal RAM. Flash data is read through the data cache that PSRAM
// shares, so a table read per pixel or per chunk stays in RAM. GCC moves a variable that nothing
// writes into flash rodata, so the RAM tables take the section attribute of esp_attr.h.
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define TONE_TABLES_RAM DRAM_ATTR
#else
#define TONE_TABLES_RAM
#endif

// The file was generated from the tone table and constants this firmware compiles. A stale file
// would hand the dither and the planner tables of another table: entry indices off by one, a
// neutral pair that is not one, patterns whose push figures were checked against other shapes or
// limits. Regenerate it with make generate in host_tests/tone_tables.
static_assert(tone_tables::count == TONEMAP_COUNT, "main/tone_tables.inc has another entry count: regenerate it");
static_assert(tone_tables::source_hash == tone_tables_source_hash(),
              "main/tone_tables.inc was generated from another tone table or other constants: regenerate it");
