// Keeps the title and icon of each browser tab, keyed by its page URL, and builds the HTML for the
// badge tab bar.
// An icon is stored and handed back as finished BMP data of a fixed TAB_ICON_PX square, so a tab
// draws without decoding its source image again.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

static const int TAB_ICON_PX = 32;
static const size_t TAB_WINDOW = 8;

struct tab_info
{
    std::string title;
    std::string icon_bmp;
    bool        placeholder = false;
};

bool tabs_load(const std::string& url, tab_info& out);
bool tabs_save(const std::string& url, const tab_info& t);

std::string tabs_icon_bmp(const std::string& file, bool partial, std::string* format = nullptr);

std::string tabs_placeholder_bmp(const std::string& host);

std::string tabs_png_url(const uint8_t* begin, const uint8_t* end);

std::string tabs_html(const std::vector<page_entry>& pages, size_t current, const std::vector<tab_info>& infos,
                      int tab_h);

std::string tabs_host(const std::string& url);

std::string html_escape(const std::string& s);
