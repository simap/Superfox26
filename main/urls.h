// Built-in page list, used while /data/urls.txt does not exist (config.h). The console's
// "url add" writes that file. Fields: URL, scroll offset in pixels, seconds on screen (0 for
// CONFIG_B26_CYCLE_SECONDS). The HTML of a page must stay under PAGE_HTML_MAX bytes (render.h).
// Offsets are where the content starts in the host harness render of each page on 2026-11-07.
#pragma once

#include "config.h"

static const page_entry PAGE_URLS[] = {

    // Plants, leaves and fruit
    {"http://app.bhencke.com/had26/strawberry.jpg", 0, 0},
    {"http://app.bhencke.com/had26/ice-plant-closeup.jpg", 0, 0},
    {"http://app.bhencke.com/had26/japanese-maple-autumn.jpg", 0, 0},
    {"http://app.bhencke.com/had26/autumn-leaves.jpg", 0, 0},
    {"http://app.bhencke.com/had26/hens-and-chicks-succulent.jpg", 0, 0},
    {"http://app.bhencke.com/had26/ice-plant-hillside.jpg", 0, 0},

    // Flowers
    {"http://app.bhencke.com/had26/red-rose-raindrops.jpg", 0, 0},
    // {"http://app.bhencke.com/had26/pink-cyclamen.jpg", 0, 0},
    {"http://app.bhencke.com/had26/orange-petals-macro.jpg", 0, 0},
    {"http://app.bhencke.com/had26/red-flower-bud.jpg", 0, 0},
    {"http://app.bhencke.com/had26/blanket-flower-dewdrops.jpg", 0, 0},
    {"http://app.bhencke.com/had26/gazania.jpg", 0, 0},
    {"http://app.bhencke.com/had26/red-carnation-petal.jpg", 0, 0},
    {"http://app.bhencke.com/had26/sunflower-petals-sky.jpg", 0, 0},
    {"http://app.bhencke.com/had26/magenta-viola.jpg", 0, 0},
    // {"http://app.bhencke.com/had26/peach-rose-dewdrops.jpg", 0, 0},
    {"http://app.bhencke.com/had26/pink-white-carnation.jpg", 0, 0},
    {"http://app.bhencke.com/had26/pink-ranunculus.jpg", 0, 0},
    {"http://app.bhencke.com/had26/pink-oxalis.jpg", 0, 0},
    {"http://app.bhencke.com/had26/barberry-blossoms.jpg", 0, 0},
    {"http://app.bhencke.com/had26/hanging-begonia-basket.jpg", 0, 0},
    {"http://app.bhencke.com/had26/pink-tea-tree-flower.jpg", 0, 0},
    {"http://app.bhencke.com/had26/pink-rockrose.jpg", 0, 0},
    // {"http://app.bhencke.com/had26/red-sunflower.jpg", 0, 0},
    {"http://app.bhencke.com/had26/red-rose-buds.jpg", 0, 0},
    {"http://app.bhencke.com/had26/african-daisy-macro.jpg", 0, 0},

    // Critters
    {"http://app.bhencke.com/had26/sea-stars-anemones.jpg", 0, 0},
    {"http://app.bhencke.com/had26/sea-nettle-jellyfish.jpg", 0, 0},
    {"http://app.bhencke.com/had26/blue-damselfly.jpg", 0, 0},
    {"http://app.bhencke.com/had26/honeybee.jpg", 0, 0},
    {"http://app.bhencke.com/had26/striped-jellyfish.jpg", 0, 0},

    // Sunsets and night sky
    // {"http://app.bhencke.com/had26/golden-ocean-sunset.jpg", 0, 0},
    // {"http://app.bhencke.com/had26/sepia-ocean-sunset.jpg", 0, 0},
    {"http://app.bhencke.com/had26/natural-bridges-sunset.jpg", 0, 0},
    {"http://app.bhencke.com/had26/aurora.jpg", 0, 0},
    {"http://app.bhencke.com/had26/milky-way.jpg", 0, 0},

    // Landscapes and cities
    {"http://app.bhencke.com/had26/seattle.jpg", 0, 0},
    {"http://app.bhencke.com/had26/golden-gate-bridge.jpg", 0, 0},
    {"http://app.bhencke.com/had26/glacier.jpg", 0, 0},

    // LED
    {"http://app.bhencke.com/had26/red-smd-led.jpg", 0, 0},


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
