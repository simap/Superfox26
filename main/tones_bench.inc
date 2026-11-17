// Timing of the tone code on the device. tones.cpp includes this at its end in a TONES_BENCH build
// (tools/idf.sh -DTONES_BENCH=1 build). main.cpp then runs tones_bench() after tones_init() and
// stops before the browser starts, so the panel is not drawn. Every line starts with BENCH and the
// last one is BENCH DONE.

#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
const int      BN = 256;
uint8_t        b_rgb[BN][3];  // random colours
float          b_raw[BN][3];  // their Oklab
float          b_lab[BN][3];  // their Oklab after the squeeze
float          b_lin[BN][3];  // and that in linear light
uint8_t        b_near[BN];    // nearest entry of b_lab
volatile float b_sink;

template <typename F> float ns_per(int reps, F f)
{
    const int64_t t0 = esp_timer_get_time();
    for(int r = 0; r < reps; r++)
        for(int i = 0; i < BN; i++) f(i);
    return static_cast<float>(esp_timer_get_time() - t0) * 1000.0f / static_cast<float>(reps * BN);
}

void line(const char* name, float ns)
{
    printf("BENCH %-36s %8.0f ns %7.0f cycles\n", name, ns, ns * 0.24f);
}

enum
{
    IMG_TEXT,
    IMG_FLAT,
    IMG_PHOTO,
    IMG_NOISE,
    IMG_N
};
const char* const k_img[IMG_N] = {"text", "gradient", "photo", "noise"};

// One row of a synthetic page. text: paper, a gray header, lines of black glyph pixels with gray
// edge pixels, every 7th line in link blue. gradient: smooth colour. photo: the gradient with 3
// counts of noise per channel. noise: every pixel a random colour.
void gen_row(int img, int y, uint8_t* rgb)
{
    for(int x = 0; x < TONE_W; x++)
    {
        uint8_t* p = rgb + 3 * x;
        const uint32_t h = hash32(static_cast<uint32_t>(x) * 7919u + static_cast<uint32_t>(y) * 104729u);
        if(img == IMG_TEXT)
        {
            p[0] = p[1] = p[2] = 255;
            if(y < 40) p[0] = p[1] = p[2] = 200;
            else if((y - 40) % 24 < 14)
            {
                const uint32_t g    = hash32(static_cast<uint32_t>(x / 3) + 4099u * static_cast<uint32_t>(y / 2));
                const bool     link = ((y - 40) / 24) % 7 == 3;
                if(g % 4 == 0)
                {
                    p[0] = p[1] = 0;
                    p[2] = link ? 238 : 0;
                }
                else if(g % 8 == 1) p[0] = p[1] = p[2] = static_cast<uint8_t>(64 + (g >> 8) % 160);
            }
        }
        else if(img == IMG_NOISE)
        {
            p[0] = static_cast<uint8_t>(h), p[1] = static_cast<uint8_t>(h >> 8), p[2] = static_cast<uint8_t>(h >> 16);
        }
        else
        {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            float       c[3] = {128.0f + 100.0f * sinf(fx / 90.0f + fy / 200.0f),
                                128.0f + 100.0f * sinf(fx / 130.0f - fy / 70.0f + 1.0f),
                                128.0f + 90.0f * sinf((fx + fy) / 160.0f + 2.0f)};
            for(int k = 0; k < 3; k++)
            {
                if(img == IMG_PHOTO) c[k] += static_cast<float>((h >> (8 * k)) % 7) - 3.0f;
                p[k] = static_cast<uint8_t>(clampf(c[k], 0.0f, 255.0f));
            }
        }
    }
}

// The dither of one whole frame, bands of 64 rows from PSRAM as render.cpp feeds it. Only the row
// calls are timed.
void run_frame(int img, const char* mode, uint8_t* band)
{
    const int   BAND = 64;
    tone_dither q;
    q.alloc(TONE_W);
    if(s_bench_bits) memset(s_cache, 0, (static_cast<size_t>(1) << s_bench_bits) * sizeof(target));
    s_bench_miss = s_bench_look = 0;
    int64_t us = 0;
    for(int y0 = 0; y0 < TONE_H; y0 += BAND)
    {
        const int n = std::min(BAND, TONE_H - y0);
        for(int y = 0; y < n; y++) gen_row(img, y0 + y, band + static_cast<size_t>(y) * TONE_W * 3);
        const int64_t t0 = esp_timer_get_time();
        for(int y = 0; y < n; y++)
            q.row(band + static_cast<size_t>(y) * TONE_W * 3, y0 + y, s_frame + static_cast<size_t>(y0 + y) * TONE_W);
        us += esp_timer_get_time() - t0;
        vTaskDelay(1);
    }
    q.release();
    printf("BENCH frame %-9s cache %-14s %6u ms, %6u lookups, %6u misses\n", k_img[img], mode,
           static_cast<unsigned>(us / 1000), static_cast<unsigned>(s_bench_look), static_cast<unsigned>(s_bench_miss));
}
} // namespace

void tones_bench()
{
    printf("BENCH start: free internal %u, largest internal block %u, free PSRAM %u\n",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // init work, each run once more
    {
        char    why[160];
        int64_t t0 = esp_timer_get_time();
        tone_plan_init(why, sizeof(why));
        printf("BENCH init tone_plan_init          %6u us\n", static_cast<unsigned>(esp_timer_get_time() - t0));
        t0 = esp_timer_get_time();
        tonemap_squeeze_begin(oklab);
        printf("BENCH init tonemap_squeeze_begin   %6u us\n", static_cast<unsigned>(esp_timer_get_time() - t0));
        t0 = esp_timer_get_time();
        build_noise();
        printf("BENCH init build_noise             %6u us\n", static_cast<unsigned>(esp_timer_get_time() - t0));
        t0 = esp_timer_get_time();
        tones_init();
        printf("BENCH init tones_init, all of it   %6u us\n", static_cast<unsigned>(esp_timer_get_time() - t0));
    }

    for(int i = 0; i < BN; i++)
    {
        const uint32_t h = hash32(static_cast<uint32_t>(i) * 2654435761u + 17u);
        b_rgb[i][0] = static_cast<uint8_t>(h), b_rgb[i][1] = static_cast<uint8_t>(h >> 8);
        b_rgb[i][2] = static_cast<uint8_t>(h >> 16);
        // a red or pink in half the slots, so the squeeze leaves chroma to search with
        if(i & 1) b_rgb[i][1] = b_rgb[i][2] = static_cast<uint8_t>(b_rgb[i][1] / 2);
        oklab(b_rgb[i][0], b_rgb[i][1], b_rgb[i][2], b_raw[i]);
        memcpy(b_lab[i], b_raw[i], sizeof(b_lab[i]));
        tonemap_squeeze(b_lab[i]);
        b_lab[i][0] = std::max(b_lab[i][0], s_lab[s_black][0]);
        oklab_to_lin(b_lab[i], b_lin[i]);
        b_near[i] = static_cast<uint8_t>(nearest(b_lab[i]));
    }

    const int R = 40;
    line("loop and one load", ns_per(R, [](int i) { b_sink = b_lin[i][0]; }));
    line("sRGB to linear, 3 powf", ns_per(R, [](int i) {
        float s = 0;
        for(int k = 0; k < 3; k++)
        {
            const float c = b_rgb[i][k] / 255.0f;
            s += c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
        }
        b_sink = s;
    }));
    line("sRGB to linear, 3 table reads", ns_per(R, [](int i) {
        b_sink = s_byte_lin[b_rgb[i][0]] + s_byte_lin[b_rgb[i][1]] + s_byte_lin[b_rgb[i][2]];
    }));
    line("cbrtf", ns_per(R, [](int i) { b_sink = cbrtf(b_lin[i][0] + 0.01f); }));
    line("oklab, table and 3 cbrtf", ns_per(R, [](int i) {
        float t[3];
        oklab(b_rgb[i][0], b_rgb[i][1], b_rgb[i][2], t);
        b_sink = t[0];
    }));
    line("tonemap_squeeze", ns_per(R, [](int i) {
        float t[3] = {b_raw[i][0], b_raw[i][1], b_raw[i][2]};
        tonemap_squeeze(t);
        b_sink = t[0];
    }));
    line("oklab_to_lin", ns_per(R, [](int i) {
        float t[3];
        oklab_to_lin(b_lab[i], t);
        b_sink = t[0];
    }));
    line("lin_to_oklab", ns_per(R, [](int i) {
        float t[3];
        lin_to_oklab(b_lin[i], t);
        b_sink = t[0];
    }));
    line("nearest, pruned by L", ns_per(R, [](int i) { b_sink = static_cast<float>(nearest(b_lab[i])); }));
    line("nearest, all entries", ns_per(R, [](int i) {
        unsigned best = 0;
        float    bd   = 1e9f;
        for(unsigned e = 0; e < N; e++)
        {
            const float d = dist(b_lab[i], s_lab[e]);
            if(d < bd) bd = d, best = e;
        }
        b_sink = static_cast<float>(best);
    }));
    for(int nb : {12, 24, 48})
    {
        char name[40];
        snprintf(name, sizeof(name), "two nearest of %d neighbours", nb);
        line(name, ns_per(R, [nb](int i) {
            const uint8_t* cand = s_nb[b_near[i]];
            unsigned       best = cand[0], next = cand[0];
            float          bd = 1e9f, nd = 1e9f;
            for(int k = 0; k < nb && k < NB; k++)
            {
                const unsigned f  = cand[k];
                const float    dd = dist(b_lab[i], s_lab[f]);
                if(dd < bd) nd = bd, next = best, bd = dd, best = f;
                else if(dd < nd) nd = dd, next = f;
            }
            b_sink = static_cast<float>(best + next);
        }));
    }
    line("two nearest of all entries", ns_per(R, [](int i) {
        unsigned best = 0, next = 0;
        float    bd = 1e9f, nd = 1e9f;
        for(unsigned f = 0; f < N; f++)
        {
            const float dd = dist(b_lab[i], s_lab[f]);
            if(dd < bd) nd = bd, next = best, bd = dd, best = f;
            else if(dd < nd) nd = dd, next = f;
        }
        b_sink = static_cast<float>(best + next);
    }));
    line("bracket", ns_per(R, [](int i) {
        uint8_t lo, hi;
        bracket(b_lab[i][0], &lo, &hi);
        b_sink = static_cast<float>(lo + hi);
    }));
    line("make_target, colour", ns_per(R, [](int i) {
        target t;
        make_target(b_rgb[i][0], b_rgb[i][1], b_rgb[i][2], &t);
        b_sink = t.lin[0];
    }));
    line("make_target, gray", ns_per(R, [](int i) {
        target t;
        make_target(b_rgb[i][0], b_rgb[i][0], b_rgb[i][0], &t);
        b_sink = t.lin[0];
    }));
    {
        uint32_t count[TONEMAP_COUNT];
        for(uint32_t& c : count) c = 1;
        static tone_plan plan;
        tone_plan_build(count, &plan);
        line("tone_plan_code", ns_per(R, [](int i) {
            b_sink = static_cast<float>(tone_plan_code(plan, static_cast<unsigned>(i) % N, TONE_PHASE_2,
                                                       static_cast<unsigned>(i) % plan.len[TONE_PHASE_2]));
        }));
        const int64_t t0 = esp_timer_get_time();
        tone_seq      s;
        for(unsigned e = 0; e < N; e++) tone_pattern_expand(k_tonemap[e].pattern, &s, nullptr, 0);
        line("tone_pattern_expand, per entry", static_cast<float>(esp_timer_get_time() - t0) * 1000.0f / static_cast<float>(N));
    }

    // whole frames, per cache
    uint8_t* band  = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(TONE_W) * 3 * 64, MALLOC_CAP_SPIRAM));
    target*  ps12  = static_cast<target*>(heap_caps_malloc(4096 * sizeof(target), MALLOC_CAP_SPIRAM));
    target*  in10  = static_cast<target*>(heap_caps_malloc(1024 * sizeof(target), MALLOC_CAP_INTERNAL));
    target*  ps10  = s_cache;
    if(!band || !ps12 || !in10) printf("BENCH frame runs skipped, no memory\n");
    else
    {
        struct
        {
            const char* name;
            target*     mem;
            int         bits;
        } const modes[] = {{"none", ps10, 0},          {"PSRAM 1024", ps10, 10},  {"PSRAM 4096", ps12, 12},
                           {"internal 1024", in10, 10}, {"internal 256", in10, 8}, {"internal 64", in10, 6}};
        for(int img = 0; img < IMG_N; img++)
            for(const auto& m : modes)
            {
                s_cache      = m.mem;
                s_bench_bits = m.bits;
                run_frame(img, m.name, band);
            }
    }
    s_cache      = ps10;
    s_bench_bits = CACHE_BITS;
    memset(s_cache, 0, CACHE_SLOTS * sizeof(target));
    heap_caps_free(band);
    heap_caps_free(ps12);
    heap_caps_free(in10);
    printf("BENCH DONE\n");
}
