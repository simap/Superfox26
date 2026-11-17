// Tab bar across the top of the panel: one tab per page in the list, with the page's favicon and title.
// Title and icon come from the page when it is rendered and are kept in /data/tabs/<hash>.bin (hash of
// the configured URL, the icon from the page after redirects), so
// every tab shows even for pages not yet visited since boot. The bar itself is HTML rendered by the
// engine (tabs_html), so it gets rounded corners and the same fonts as the pages.
#pragma once

#include <string>
#include <vector>

#include "config.h"

struct tab_info
{
    std::string title;               // from <title>, or the host name
    std::string icon_bmp;            // 32x32 32-bit BMP file bytes, empty when none is stored
    bool        placeholder = false; // icon_bmp is tabs_placeholder_bmp, the real icon is still wanted
};

// Reads the stored info for url. False when none is stored or the file is unreadable. A file cut off
// inside the icon gives the title and no icon, and true.
bool tabs_load(const std::string& url, tab_info& out);
// Stores info for url through a temporary file and a rename, so a reset during the write leaves the
// previous file. False when the write failed.
bool tabs_save(const std::string& url, const tab_info& t);

// A 32x32 32-bit BMP (alpha from the image's transparency) from an icon file, the format sniffed from
// the bytes: ICO (the entry nearest 32 px and at least 32, PNG or BMP inside), PNG, GIF, BMP, WebP or
// JPEG. The image is box filtered to fit 32x32 with its aspect kept. partial: file is a prefix of the
// resource, so only an ICO is decoded, from the entries inside the prefix. format, when given, receives
// a short description such as "png 180x180". Empty when the file does not decode.
std::string tabs_icon_bmp(const std::string& file, bool partial, std::string* format = nullptr);

// A 32x32 32-bit BMP for a page without an icon: a black-bordered white box with the first letter or
// digit of host in black.
std::string tabs_placeholder_bmp(const std::string& host);

// HTML for the bar: pages in order, the current one filled black. infos runs parallel to pages. tab_h
// is the bar height in pixels.
std::string tabs_html(const std::vector<page_entry>& pages, size_t current, const std::vector<tab_info>& infos,
                      int tab_h);

// Host name of a URL, for a tab with no title.
std::string tabs_host(const std::string& url);

// s with &, <, >, " and ' replaced by entities.
std::string html_escape(const std::string& s);
