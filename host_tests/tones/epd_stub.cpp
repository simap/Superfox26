// The epd97 calls tones_draw() makes, defined so tones.cpp links on the host. The harness never
// calls tones_draw(), so each of these aborts.

#include <cstdio>
#include <cstdlib>

#include "epd97.h"

namespace
{
[[noreturn]] void no_panel(const char* fn)
{
    fprintf(stderr, "%s: no panel on the host\n", fn);
    abort();
}
} // namespace

uint8_t* epd_plane_bw(void)
{
    no_panel(__func__);
}

uint8_t* epd_plane_red(void)
{
    no_panel(__func__);
}

bool refresh_display(epd_refresh_t)
{
    no_panel(__func__);
}

esp_err_t epd_verb_erase(const char*, uint8_t, epd_erase_stats_t*)
{
    no_panel(__func__);
}

esp_err_t epd_verb_open_table(uint8_t, uint8_t, epd_verb_table_t)
{
    no_panel(__func__);
}

esp_err_t epd_verb_pull(unsigned, bool, uint32_t*)
{
    no_panel(__func__);
}

esp_err_t epd_verb_pass(unsigned, uint32_t*)
{
    no_panel(__func__);
}

void epd_verb_close(void)
{
    no_panel(__func__);
}

void epd_clear(void)
{
    no_panel(__func__);
}
