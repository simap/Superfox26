// The generated file tone_tables.inc holds the tone tables in the tone_tables namespace.
// The two static_asserts fail the build when the generated file does not match the current entry
// count or the hashed source: the tone map entries, the TONEMAP_* constants, and the EPD and tone
// plan drive constants.
// Regenerate tone_tables.inc then.
// The hash does not cover every generator input, so a change to an unhashed input such as KEY_MASK
// still passes these checks and only the host test finds the mismatch.
#pragma once

#include <array>
#include <cstdint>

#include "tone_fx.h"
#include "tone_fx_bracket.h"
#include "tone_fx_search_internal.h"
#include "tone_fx_squeeze_internal.h"
#include "tone_photo.h"
#include "tone_plan.h"
#include "tone_tables_source.h"
#include "tonemap.h"

#include "tone_tables.inc"

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define TONE_TABLES_RAM DRAM_ATTR
#else
#define TONE_TABLES_RAM
#endif

static_assert(tone_tables::count == TONEMAP_COUNT, "main/tone_tables.inc has another entry count: regenerate it");
static_assert(tone_tables::source_hash == tone_tables_source_hash(),
              "main/tone_tables.inc was generated from another tone table or other constants: regenerate it");
