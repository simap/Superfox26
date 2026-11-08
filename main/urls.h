// Built-in page list, used while /data/urls.txt does not exist (config.h). The console's
// "url add" writes that file. Fields: URL, scroll offset in pixels, seconds on screen (0 for
// CONFIG_B26_CYCLE_SECONDS). The HTML of a page must stay under PAGE_HTML_MAX bytes (render.h).
// Offsets are where the content starts in the host harness render of each page on 2026-11-07.
#pragma once

#include "config.h"

static const page_entry PAGE_URLS[] = {

    // {"https://text.npr.org/", 0, 0},
    // {"https://c.xkcd.com/random/comic/", 225, 0},
    // {"https://lite.cnn.com/", 0, 0},
    // {"https://c.xkcd.com/random/comic/", 225, 0},
    // {"https://68k.news/", 240, 0},
    
    {"https://c.xkcd.com/random/comic/", 225, 0},
    {"https://lwn.net/", 260, 0},
    // {"https://www.cbc.ca/lite/news", 130, 0},
    // {"https://wttr.in/Pasadena", 0, 0},
    // {"https://plaintextsports.com/", 0, 0},
    {"https://c.xkcd.com/random/comic/", 225, 0},
    {"https://forecast.weather.gov/MapClick.php?lat=34.1478&lon=-118.1445&unrestricted=1", 0, 0},
    
    // {"https://en.wikipedia.org/wiki/Electronic_paper", 150, 0},
    // {"https://c.xkcd.com/random/comic/", 225, 0},
    {"https://c.xkcd.com/random/comic/", 225, 0},
    {"https://hackaday.com/", 20, 0},
    {"https://hackaday.com/blog/", 20, 0},
    
    
};
