// Host model of the epd97 calls tones_draw() makes, see epd_stub.h. The model follows epd97.h: an
// open latches one table and loads zero planes, a pass sends both planes then pulls, a pull without
// a send plays the plane registers as they are, and the COG swaps its two plane registers after
// every pull, so codes 1 and 2 trade places and 0 and 3 stay. A code is (red bit << 1) | bw bit.

#include "epd_stub.h"

#include <algorithm>
#include <cstring>

#include "epd97.h"
#include "tonemap.h"

epd_stub_log g_stub;

namespace
{
uint8_t          s_bw[EPD_PLANE_BYTES], s_red[EPD_PLANE_BYTES];   // the framebuffer planes
uint8_t          s_reg_bw[EPD_PLANE_BYTES], s_reg_red[EPD_PLANE_BYTES]; // the COG's plane registers
bool             s_open  = false;
epd_verb_table_t s_table = EPD_VERB_TABLE_VERB;
uint8_t          s_unit  = 0;
unsigned         s_k     = 0; // pulls played in the open session

void pull_once()
{
    epd_stub_log&    g  = g_stub;
    const tone_phase ph = s_table == EPD_VERB_TABLE_PHASE1 ? TONE_PHASE_1 : TONE_PHASE_2;
    if(g.frame && g.count)
    {
        uint8_t want[TONEMAP_COUNT];
        for(unsigned e = 0; e < TONEMAP_COUNT; e++)
            want[e] = g.count[e] && s_k < g.plan.len[ph] ? tone_plan_code(g.plan, e, ph, s_k) : 0xFF;
        for(size_t p = 0; p < static_cast<size_t>(EPD_W) * EPD_H; p++)
        {
            const uint8_t m    = static_cast<uint8_t>(0x80u >> (p & 7));
            const uint8_t code = static_cast<uint8_t>(((s_reg_red[p >> 3] & m) ? 2 : 0) | ((s_reg_bw[p >> 3] & m) ? 1 : 0));
            g.mismatches += code != want[g.frame[p]];
        }
    }
    g.pulls[ph]++;
    s_k++;
    std::swap_ranges(s_reg_bw, s_reg_bw + EPD_PLANE_BYTES, s_reg_red);
}
} // namespace

uint8_t* epd_plane_bw(void)
{
    return s_bw;
}

uint8_t* epd_plane_red(void)
{
    return s_red;
}

bool refresh_display(epd_refresh_t mode)
{
    s_open = false;
    g_stub.refreshes++;
    g_stub.refresh_mode = mode;
    return true;
}

esp_err_t epd_verb_open_table(uint8_t unit_frames, uint8_t vcom_level, epd_verb_table_t table)
{
    if(g_stub.no_tables) return ESP_ERR_NOT_SUPPORTED;
    epd_stub_log& g = g_stub;
    g.opens++;
    // Phase 1 opens EPD_VERB_TABLE_PHASE1 before any phase 2 pull, phase 2 EPD_VERB_TABLE_SEQ at
    // TONE_NUDGE_FRAMES after every phase 1 pull. VCOM is ground.
    const bool p1 = table == EPD_VERB_TABLE_PHASE1 && g.pulls[TONE_PHASE_2] == 0;
    const bool p2 = table == EPD_VERB_TABLE_SEQ && unit_frames == TONE_NUDGE_FRAMES &&
                    g.pulls[TONE_PHASE_1] == g.plan.len[TONE_PHASE_1];
    if(vcom_level != 0 || !(p1 || p2)) g.bad_open++;
    s_open  = true;
    s_table = table;
    s_unit  = unit_frames;
    s_k     = 0;
    memset(s_reg_bw, 0, sizeof(s_reg_bw));
    memset(s_reg_red, 0, sizeof(s_reg_red));
    return ESP_OK;
}

esp_err_t epd_verb_pull(unsigned pulls, bool resend, uint32_t* panel_ms_total)
{
    if(panel_ms_total) *panel_ms_total = 0;
    if(!s_open) return ESP_ERR_INVALID_STATE;
    if(pulls == 0 || pulls > EPD_VERB_PULLS_MAX) return ESP_ERR_INVALID_ARG;
    if(resend)
    {
        memcpy(s_reg_bw, s_bw, sizeof(s_bw));
        memcpy(s_reg_red, s_red, sizeof(s_red));
        g_stub.sends++;
    }
    else if(s_k == 0)
        g_stub.first_kept++;
    for(unsigned i = 0; i < pulls; i++) pull_once();
    return ESP_OK;
}

esp_err_t epd_verb_pass(unsigned pulls, uint32_t* panel_ms_total)
{
    return epd_verb_pull(pulls, true, panel_ms_total);
}

void epd_verb_close(void)
{
    if(!s_open) return;
    s_open = false;
    g_stub.closes++;
}

bool epd_verb_is_open(void)
{
    return s_open;
}

epd_verb_table_t epd_verb_table(void)
{
    return s_open ? s_table : EPD_VERB_TABLE_VERB;
}

uint8_t epd_verb_unit(void)
{
    return s_open ? s_unit : 0;
}

void epd_clear(void)
{
    memset(s_bw, 0, sizeof(s_bw));
    memset(s_red, 0, sizeof(s_red));
    g_stub.clears++;
}
