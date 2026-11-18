// Renders a web page into a tone frame for the display.
// render_page fetches a URL, lays it out with litehtml, and dithers the pixels to the display tone
// values.
// It draws the page in horizontal bands to stay within the limited PSRAM.
// Call render_init and check that it returns true before any other function here; it starts HTTP
// fetch and loads the fonts the other calls use.
// frame points to a TONE_W by TONE_H buffer of tone values.
// render_page and render_html clear the whole buffer first, while render_page leaves the top TAB_H
// rows empty and render_tabs fills only those rows, so a page and its tab bar share one frame.
#pragma once

#include <cstdint>
#include <string>

#include <vector>

#include "config.h"

static const size_t PAGE_HTML_MAX = 700 * 1024;

static const int TAB_H = 38;

struct render_stats
{
    size_t html_bytes = 0;
    int    doc_h      = 0;
    int    offset     = 0;
    int    fetch_ms = 0, parse_ms = 0, layout_ms = 0, images_ms = 0, draw_ms = 0;
    int    dither_ms = 0;
    size_t images = 0, img_fail = 0;
    size_t img_probes = 0, img_cache_hits = 0;
    size_t img_in_view = 0, img_fetched_bytes = 0, img_skipped = 0;
    bool   relayout = false;
    size_t psram_low = 0;
    bool   cancelled = false;
};

bool render_init();

// cancel, when given, is asked before the parse, before each image fetch and before each band. Once it
// returns true the render stops at those points and returns false with st.cancelled set. Layout runs
// inside litehtml and is not cut, but its image probes are skipped.
bool render_page(const std::string& url, int offset, uint8_t* frame, render_stats& st, std::string& err,
                 bool (*cancel)() = nullptr);

bool render_tabs(const std::vector<page_entry>& pages, size_t current, uint8_t* frame);

bool render_html(const std::string& html, uint8_t* frame);
