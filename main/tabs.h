// Tab bar across the top of the panel: one tab per page in the list, with the page's favicon and title.
// Title and icon come from the page when it is rendered and are kept in /data/tabs/<hash>.bin, so
// every tab shows even for pages not yet visited since boot. The bar itself is HTML rendered by the
// engine (tabs_html), so it gets rounded corners and the same fonts as the pages.
#pragma once

#include <string>
#include <vector>

#include "config.h"

struct tab_info
{
    std::string title;    // from <title>, or the host name
    std::string icon_bmp; // 16x16 32-bit BMP file bytes, empty when no icon could be fetched
};

// Reads the stored info for url. False when none is stored.
bool tabs_load(const std::string& url, tab_info& out);
// Stores info for url. False when the write failed.
bool tabs_save(const std::string& url, const tab_info& t);

// A 16x16 32-bit BMP (alpha from the image's transparency) from an icon file: ICO (PNG or BMP entries),
// PNG, GIF, BMP, WebP or JPEG. Empty when the file does not decode.
std::string tabs_icon_bmp(const std::string& file);

// HTML for the bar: pages in order, the current one filled black. infos runs parallel to pages. tab_h
// is the bar height in pixels.
std::string tabs_html(const std::vector<page_entry>& pages, size_t current, const std::vector<tab_info>& infos,
                      int tab_h);

// Host name of a URL, for a tab with no title.
std::string tabs_host(const std::string& url);

// s with &, <, >, " and ' replaced by entities.
std::string html_escape(const std::string& s);
