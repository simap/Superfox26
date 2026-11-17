// The tone benchmark without the panel. It runs tones_init() and then tones_bench(), whose last
// line is BENCH DONE.
//
// A build without TONES_BENCH links the tone code as the firmware does, for its sizes and symbols:
// it runs tones_init(), one white row of the dither and tones_solid_gray(), keeps tones_draw()
// linked without calling it, and prints BENCH DONE.
#include <cstdio>
#include <cstring>

#include "tones.h"

extern "C" void app_main(void)
{
    if(!tones_init())
    {
        printf("BENCH tones_init failed\n");
        return;
    }
#ifdef TONES_BENCH
    tones_bench();
#else
    static uint8_t rgb[3 * TONE_W + 16];
    memset(rgb, 255, sizeof(rgb));
    tone_dither d;
    d.alloc(TONE_W);
    d.row(rgb, 0, tones_frame());
    d.release();
    bool (*volatile draw)(const uint8_t*, tone_draw_stats*) = tones_draw;
    printf("BENCH tones_init ok, white row entry %u, solid gray 174 -> %d, draw at %p\nBENCH DONE\n",
           static_cast<unsigned>(tones_frame()[0]), tones_solid_gray(174), reinterpret_cast<void*>(draw));
#endif
}
