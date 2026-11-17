// Fetch a page, lay it out with litehtml and draw the panel window into the tone frame.
#pragma once

#include <cstdint>
#include <string>

#include <vector>

#include "config.h"

// HTML above this size is refused before parse. Parse grows the heap to about 10 times the HTML
// bytes before the first budget check runs.
static const size_t PAGE_HTML_MAX = 700 * 1024;

// Height of the tab bar at the top of the panel. The page window is the rest.
static const int TAB_H = 38;

struct render_stats
{
    size_t html_bytes = 0;
    int    doc_h      = 0;
    int    offset     = 0; // first document row shown, after clamping
    int    fetch_ms = 0, parse_ms = 0, layout_ms = 0, images_ms = 0, draw_ms = 0;
    int    dither_ms = 0; // the part of draw_ms in tone_dither::row()
    size_t images = 0, img_fail = 0;      // image references, and references whose size or file failed
    size_t img_probes = 0, img_cache_hits = 0; // how each reference got its size
    size_t img_in_view = 0, img_fetched_bytes = 0, img_skipped = 0; // the window's images
    bool   relayout = false; // a fetched image disagreed with its probed or cached size
    size_t psram_low = 0; // least free PSRAM seen during the page
};

// Initialise fonts and the HTTP client. Call once. False when the fonts partition is unusable.
bool render_init();

// Render url into rows TAB_H to 671 of the tone frame (TONE_W x TONE_H entry indices, tones.h),
// starting at document row offset, clamped so the window stays inside the document. The frame is
// cleared to paper first. Stores the page's title and favicon for the tab bar. On failure err holds a
// one-line reason and the frame is left cleared.
bool render_page(const std::string& url, int offset, uint8_t* frame, render_stats& st, std::string& err);

// Render the tab bar into frame rows 0 to TAB_H - 1: one tab per page, the current one filled.
bool render_tabs(const std::vector<page_entry>& pages, size_t current, uint8_t* frame);
