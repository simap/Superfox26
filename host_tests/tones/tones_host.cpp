// Host harness for the tone dither in main/tones.cpp. Renders test images through tone_dither at
// TONE_W wide and writes per test the input, the entry frame in the entries' colours, and a 4x crop
// of that frame. Prints the entry histogram and deviation figures. Plays each frame through
// tones_draw() into the driver model of epd_stub.cpp and checks every pull. Runs the float dither of
// main/tones_float_ref.inc on the same image and compares the two. See README.md.
//
// usage: tones_host OUTDIR [TONE_IMAGE.png ...]
//        tones_host --compare SWEEPDIR CONFIG ...
// TONES_FONT in the environment overrides the text test's face.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "epd97.h"
#include "epd_stub.h"
#include "tone_plan.h"
#include "tonemap.h"
#include "tonemap_squeeze.h"
#include "tones.h"

extern "C" {
#include "lodepng.h"
}

#if HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace
{
const int      W      = TONE_W;
const unsigned N      = static_cast<unsigned>(TONEMAP_COUNT);
const int      CROP_W = 240, CROP_H = 160, CROP_S = 4;
const int      DARK_W = 160, DARK_H = 120; // the dark crop, also 4x
const int      BAND   = 32; // ramp table column width in pixels
const int      BLOCK  = 8;  // texture block side
// Bands whose per-pixel residual standard deviation is below this are near solid. They get no
// block ratio.
const double   BLOCK_SD_MIN = 0.005;

// Deviation thresholds in Oklab units, against the pixel's target before the carried error.
const float FAR_L = 0.15f; // above the widest neutral gap, 0.1275 in L
const float FAR_C = 0.06f; // a, b distance
const float FLAT  = 0.02f; // weighted distance within the 7x7 window for a flat pixel
// tones.cpp NEUTRAL_C: a target with less a, b chroma after the squeeze is neutral.
const float NEUTRAL_C = 1e-3f;
const float LUMA[3]   = {0.2126f, 0.7152f, 0.0722f};

#if HAVE_FREETYPE
const char* DEFAULT_FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"; // tools/stage_fonts.py sans
#endif

// ---------------------------------------------------------------- images

struct image
{
    int                  h = 0;
    std::vector<uint8_t> rgb; // W x h x 3, then 16 bytes the dither's sRGB decode may read
};

image blank(int h)
{
    image im;
    im.h = h;
    im.rgb.assign(static_cast<size_t>(W) * h * 3 + 16, 255);
    return im;
}

uint8_t* px(image& im, int x, int y)
{
    return &im.rgb[(static_cast<size_t>(y) * W + x) * 3];
}

void put(image& im, int x, int y, int r, int g, int b)
{
    uint8_t* p = px(im, x, y);
    p[0]       = static_cast<uint8_t>(r);
    p[1]       = static_cast<uint8_t>(g);
    p[2]       = static_cast<uint8_t>(b);
}

// render.cpp blend(): c over the pixel at a of 255, integer mix of the sRGB bytes.
void blend(image& im, int x, int y, const uint8_t c[3], int a)
{
    if(x < 0 || x >= W || y < 0 || y >= im.h || a <= 0) return;
    uint8_t* d = px(im, x, y);
    for(int i = 0; i < 3; i++)
        d[i] = a >= 255 ? c[i] : static_cast<uint8_t>((c[i] * a + d[i] * (255 - a)) / 255);
}

#if HAVE_FREETYPE
void fill_rect(image& im, int x0, int y0, int x1, int y1, const uint8_t c[3])
{
    for(int y = y0; y < y1; y++)
        for(int x = x0; x < x1; x++) blend(im, x, y, c, 255);
}
#endif

bool save_png(const std::string& path, const uint8_t* rgb, int w, int h)
{
    unsigned err = lodepng_encode24_file(path.c_str(), rgb, static_cast<unsigned>(w), static_cast<unsigned>(h));
    if(err) fprintf(stderr, "%s: %s\n", path.c_str(), lodepng_error_text(err));
    return !err;
}

// ---------------------------------------------------------------- the tone system, as tones.cpp computes it

float    g_lab[TONEMAP_COUNT][3]; // entry Oklab
float    g_lin[TONEMAP_COUNT][3]; // entry linear light
uint8_t  g_rgb[TONEMAP_COUNT][3]; // entry sRGB, the simulated panel colour
bool     g_neutral[TONEMAP_COUNT]; // pattern without r or R
unsigned g_black = 0;
float    g_byte_lin[256];

void oklab_lin(const float c[3], float out[3])
{
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void lin_of_oklab(const float lab[3], float out[3])
{
    float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    l       = l * l * l;
    m       = m * m * m;
    s       = s * s * s;
    out[0]  = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1]  = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2]  = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

void oklab_rgb(uint8_t r, uint8_t g, uint8_t b, float out[3])
{
    const float c[3] = {g_byte_lin[r], g_byte_lin[g], g_byte_lin[b]};
    oklab_lin(c, out);
}

uint8_t srgb_byte(float v)
{
    v = std::max(0.0f, std::min(1.0f, v));
    v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
    return static_cast<uint8_t>(255.0f * v + 0.5f);
}

float luma(const float* c)
{
    return LUMA[0] * c[0] + LUMA[1] * c[1] + LUMA[2] * c[2];
}

const char* name_of(unsigned e)
{
    return e == TONE_PAPER ? "paper" : e == g_black ? "black" : k_tonemap[e].pattern;
}

int entry_of(const char* pattern)
{
    for(unsigned e = 0; e < N; e++)
        if(strcmp(k_tonemap[e].pattern, pattern) == 0) return static_cast<int>(e);
    return -1;
}

bool analysis_init()
{
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        g_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for(unsigned e = 0; e < N; e++)
    {
        g_lab[e][0]  = k_tonemap[e].L, g_lab[e][1] = k_tonemap[e].a, g_lab[e][2] = k_tonemap[e].b;
        g_neutral[e] = strpbrk(k_tonemap[e].pattern, "rR") == nullptr;
        lin_of_oklab(g_lab[e], g_lin[e]);
        for(int c = 0; c < 3; c++) g_rgb[e][c] = srgb_byte(g_lin[e][c]);
    }
    const int eb = entry_of(TONEMAP_BLACK_PATTERN);
    if(eb < 0) return false;
    g_black = static_cast<unsigned>(eb);
    tonemap_squeeze_begin(oklab_rgb);
    return true;
}

// The target tone_dither::row forms for one RGB pixel before it adds the carried error, in Oklab:
// the colour through tonemap_squeeze(), L held at the black entry's or above. True when it is
// neutral, then its a and b are set to 0.
bool pixel_target(const uint8_t* c, float t[3])
{
    oklab_rgb(c[0], c[1], c[2], t);
    tonemap_squeeze(t);
    t[0]               = std::max(t[0], g_lab[g_black][0]);
    const bool neutral = t[1] * t[1] + t[2] * t[2] < NEUTRAL_C * NEUTRAL_C;
    if(neutral) t[1] = t[2] = 0.0f;
    return neutral;
}

// The target's linear light. A neutral target's is L cubed on all three channels.
void target_lin(const float t[3], float out[3])
{
    lin_of_oklab(t, out);
}

float wdist2(const float* a, const float* b)
{
    float dl = a[0] - b[0];
    float da = (a[1] - b[1]) * TONEMAP_CIE_CHROMA_WEIGHT;
    float db = (a[2] - b[2]) * TONEMAP_CIE_CHROMA_WEIGHT;
    return dl * dl + da * da + db * db;
}

// Ramp image column x to 0..255.
int ramp_at(int x)
{
    return (x * 255 + (W - 1) / 2) / (W - 1);
}

float gray_target_l(int v)
{
    const uint8_t c[3] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v), static_cast<uint8_t>(v)};
    float         t[3];
    pixel_target(c, t);
    return t[0];
}

// The neutral entries, lightest first, and the midpoint of the widest L gap between neighbours
// whose upper entry is at or below l_max.
std::vector<unsigned> neutrals()
{
    std::vector<unsigned> nt;
    for(unsigned e = 0; e < N; e++)
        if(g_neutral[e]) nt.push_back(e);
    std::sort(nt.begin(), nt.end(), [](unsigned a, unsigned b) { return g_lab[a][0] > g_lab[b][0]; });
    return nt;
}

float widest_gap_mid(float l_max)
{
    const std::vector<unsigned> nt = neutrals();
    float                       best = 0, mid = 0.5f;
    for(size_t i = 0; i + 1 < nt.size(); i++)
    {
        const float hi = g_lab[nt[i]][0], lo = g_lab[nt[i + 1]][0];
        if(hi <= l_max && hi - lo > best) best = hi - lo, mid = 0.5f * (hi + lo);
    }
    return mid;
}

// Column of the white to black ramp whose gray target has Oklab lightness L.
int gray_x_of_l(float l)
{
    int bx = 0;
    for(int x = 1; x < W; x++)
        if(fabsf(gray_target_l(255 - ramp_at(x)) - l) < fabsf(gray_target_l(255 - ramp_at(bx)) - l)) bx = x;
    return bx;
}

// ---------------------------------------------------------------- tests

struct test_case
{
    std::string name;
    image       im;
    int         crop_x = 0, crop_y = 0;
    int         dark_x = 0, dark_y = 0; // origin of the DARK_W x DARK_H crop
    bool        ramp   = false;
};

struct texture
{
    double checker = 0; // share of 2x2 cells that are a checkerboard of two entries
    double run     = 0; // mean run of one entry along rows
    double ratio   = 0; // block mean deviation over the expected, -1 when near solid
};

struct summary_row
{
    std::string name;
    unsigned    entries = 0, pulls1 = 0, pulls2 = 0, sends = 0;
    size_t      play_bad = 0; // tones_draw() pulls or codes that differ from the plan, see play_check()
    size_t      far = 0, far_iso = 0, flat = 0, tint = 0, white = 0, white_off = 0, white_off_flat = 0, black = 0,
           black_off = 0;
    double  worst_seen = 0;   // ramps: largest band |seen L - tgt L|
    double  worst_seen_c = 0; // ramps: largest band a, b distance of seen from tgt
    double  worst_y = 0;      // ramps: largest band |entry luminance - target luminance|, linear
    int     solid_px = 0;     // ramps: columns in solid runs >= 8 px
    int     solid_max = 0;    // ramps: longest solid run
    double  solid_reach = 0;  // ramps: largest |tgt L - entry L| inside a solid run
    texture tex;              // ramps: mean over bands, else the whole frame
    double  ms = 0;           // dither time on the host
    // The float dither of main/tones_float_ref.inc on the same image.
    double  ref_differ = 0;     // share of pixels whose entry differs from the fixed point dither's
    double  ref_worst_seen = 0; // ramps: as worst_seen, for the float dither
    double  ref_worst_y = 0;    // ramps: as worst_y, for the float dither
    double  frame_dy = 0, ref_frame_dy = 0; // mean entry luminance minus mean target luminance, frame
    double  ref_ms = 0;
    double  checker_pct() const { return 100 * tex.checker; }
};

// Texture of the entry frame over columns x0 to x1 - 1, all rows. The block ratio compares the
// standard deviation of the 8x8 block means of (entry L - target L) with the deviation expected
// for 64 independent pixels, the per-pixel deviation over 8. 1 is white noise, below 1 an even
// spread, above 1 clumps.
texture texture_of(const std::vector<uint8_t>& idx, const std::vector<float>& tg, int h, int x0, int x1)
{
    texture t;
    size_t  cells = 0, checks = 0, runs = 0, n = 0;
    for(int y = 0; y < h; y++)
    {
        const uint8_t* r = &idx[static_cast<size_t>(y) * W];
        for(int x = x0; x < x1; x++)
        {
            n++;
            runs += x == x0 || r[x] != r[x - 1];
            if(x + 1 >= x1 || y + 1 >= h) continue;
            const uint8_t* s = r + W;
            cells++;
            checks += r[x] == s[x + 1] && r[x + 1] == s[x] && r[x] != r[x + 1];
        }
    }
    t.checker = cells ? static_cast<double>(checks) / cells : 0;
    t.run     = runs ? static_cast<double>(n) / runs : 0;
    double s1 = 0, s2 = 0, b1 = 0, b2 = 0;
    size_t np = 0, nb = 0;
    for(int by = 0; by + BLOCK <= h; by += BLOCK)
        for(int bx = x0; bx + BLOCK <= x1; bx += BLOCK)
        {
            double m = 0;
            for(int y = by; y < by + BLOCK; y++)
                for(int x = bx; x < bx + BLOCK; x++)
                {
                    const size_t p = static_cast<size_t>(y) * W + x;
                    const double r = g_lab[idx[p]][0] - tg[3 * p];
                    m += r, s1 += r, s2 += r * r, np++;
                }
            m /= BLOCK * BLOCK;
            b1 += m, b2 += m * m, nb++;
        }
    t.ratio = -1;
    if(np && nb)
    {
        const double sd_px = sqrt(std::max(0.0, s2 / np - (s1 / np) * (s1 / np)));
        const double sd_bl = sqrt(std::max(0.0, b2 / nb - (b1 / nb) * (b1 / nb)));
        if(sd_px >= BLOCK_SD_MIN) t.ratio = sd_bl / (sd_px / BLOCK);
    }
    return t;
}

// A cw x ch crop of the panel image at (x, y), CROP_S times nearest neighbour.
std::vector<uint8_t> crop_of(const std::vector<uint8_t>& panel, int h, int x, int y, int cw, int ch)
{
    const int cx = std::max(0, std::min(x, W - cw));
    const int cy = std::max(0, std::min(y, h - ch));
    std::vector<uint8_t> crop(static_cast<size_t>(cw * CROP_S) * ch * CROP_S * 3);
    for(int yy = 0; yy < ch * CROP_S; yy++)
        for(int xx = 0; xx < cw * CROP_S; xx++)
            memcpy(&crop[(static_cast<size_t>(yy) * cw * CROP_S + xx) * 3],
                   &panel[((static_cast<size_t>(cy + yy / CROP_S)) * W + cx + xx / CROP_S) * 3], 3);
    return crop;
}

std::vector<std::pair<unsigned, size_t>> top_entries(const size_t* count, unsigned k)
{
    std::vector<std::pair<unsigned, size_t>> v;
    for(unsigned e = 0; e < N; e++)
        if(count[e]) v.push_back({e, count[e]});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if(v.size() > k) v.resize(k);
    return v;
}

// Plays the entry frame (h rows, the rest paper) through tones_draw() into the driver model and
// checks it against the plan: every pull of both phases, each pixel's code at each pull equal to
// tone_plan_code() of its entry after the COG's plane swap, the first pull of each session with a
// plane send, the sessions on the right table and unit in phase order, and at the end the session
// closed and the planes clear.
void play_check(const std::vector<uint8_t>& idx, int h, summary_row& s)
{
    std::vector<uint8_t> frame(static_cast<size_t>(TONE_W) * TONE_H, TONE_PAPER);
    memcpy(frame.data(), idx.data(), static_cast<size_t>(W) * h);
    uint32_t count[TONEMAP_COUNT] = {0};
    for(uint8_t e : frame) count[e]++;
    tone_plan plan;
    tone_plan_build(count, &plan);
    g_stub       = epd_stub_log();
    g_stub.frame = frame.data();
    g_stub.plan  = plan;
    g_stub.count = count;
    tone_draw_stats ds;
    const bool      ok    = tones_draw(frame.data(), &ds);
    bool            clear = true;
    for(size_t i = 0; i < EPD_PLANE_BYTES; i++) clear = clear && !epd_plane_bw()[i] && !epd_plane_red()[i];
    const epd_stub_log& g = g_stub;
    s.pulls1 = g.pulls[TONE_PHASE_1], s.pulls2 = g.pulls[TONE_PHASE_2], s.sends = g.sends;
    s.play_bad = g.mismatches + g.first_kept + g.bad_open + !ok + !clear + epd_tone_is_open() +
                 (g.pulls[TONE_PHASE_1] != plan.len[TONE_PHASE_1]) + (g.pulls[TONE_PHASE_2] != plan.len[TONE_PHASE_2]) +
                 (ds.pulls1 != plan.len[TONE_PHASE_1]) + (ds.pulls2 != plan.len[TONE_PHASE_2]);
    printf("  play: plan %u + %u pulls (B %u), played %u + %u, %u sends, %u opens, %u closes, mismatched pixel "
           "pulls %zu, first pulls kept %u, bad opens %u, %s, session %s, planes %s => %s\n",
           plan.len[TONE_PHASE_1], plan.len[TONE_PHASE_2], plan.mid, g.pulls[TONE_PHASE_1], g.pulls[TONE_PHASE_2],
           g.sends, g.opens, g.closes, g.mismatches, g.first_kept, g.bad_open, ok ? "ok" : "failed",
           epd_tone_is_open() ? "open" : "closed", clear ? "clear" : "not clear", s.play_bad ? "FAIL" : "pass");
}

void run(const test_case& tc, const std::string& outdir, std::vector<summary_row>& sum)
{
    const int    h = tc.im.h;
    const size_t n = static_cast<size_t>(W) * h;

    std::vector<uint8_t> idx(n);
    tone_dither          q;
    const auto           t0 = std::chrono::steady_clock::now();
    q.alloc(W);
    for(int y = 0; y < h; y++) q.row(&tc.im.rgb[static_cast<size_t>(y) * W * 3], y, &idx[static_cast<size_t>(y) * W]);
    q.release();
    summary_row s;
    s.name = tc.name;
    s.ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::vector<uint8_t> ref(n);
    tone_dither_float    qf;
    const auto           t1 = std::chrono::steady_clock::now();
    qf.alloc(W);
    for(int y = 0; y < h; y++) qf.row(&tc.im.rgb[static_cast<size_t>(y) * W * 3], y, &ref[static_cast<size_t>(y) * W]);
    qf.release();
    s.ref_ms      = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    size_t differ = 0;
    for(size_t p = 0; p < n; p++) differ += idx[p] != ref[p];
    s.ref_differ = static_cast<double>(differ) / n;

    std::vector<uint8_t> panel(n * 3);
    for(size_t p = 0; p < n; p++) memcpy(&panel[3 * p], g_rgb[idx[p]], 3);
    const int         cx   = std::max(0, std::min(tc.crop_x, W - CROP_W));
    const int         cy   = std::max(0, std::min(tc.crop_y, h - CROP_H));
    const int         dx   = std::max(0, std::min(tc.dark_x, W - DARK_W));
    const int         dy   = std::max(0, std::min(tc.dark_y, h - DARK_H));
    const std::string base = outdir + "/" + tc.name;
    save_png(base + "_input.png", tc.im.rgb.data(), W, h);
    save_png(base + "_panel.png", panel.data(), W, h);
    save_png(base + "_crop4x.png", crop_of(panel, h, cx, cy, CROP_W, CROP_H).data(), CROP_W * CROP_S, CROP_H * CROP_S);
    save_png(base + "_dark4x.png", crop_of(panel, h, dx, dy, DARK_W, DARK_H).data(), DARK_W * CROP_S, DARK_H * CROP_S);

    printf("\n== %s: %dx%d, crop %dx%d at (%d, %d), dark crop %dx%d at (%d, %d), dither %.1f ms, float dither %.1f ms\n",
           tc.name.c_str(), W, h, CROP_W, CROP_H, cx, cy, DARK_W, DARK_H, dx, dy, s.ms, s.ref_ms);
    printf("  float dither: %zu pixels of %zu (%.2f%%) take another entry\n", differ, n, 100.0 * s.ref_differ);

    // histogram
    size_t count[TONEMAP_COUNT] = {0};
    for(size_t p = 0; p < n; p++) count[idx[p]]++;
    printf("  idx  pattern             L      a      b    pixels   share\n");
    for(unsigned e = 0; e < N; e++)
    {
        if(!count[e]) continue;
        s.entries++;
        printf("  %3u  %-16s %6.4f %+7.4f %+7.4f %9zu %6.2f%%\n", e, name_of(e), g_lab[e][0], g_lab[e][1], g_lab[e][2],
               count[e], 100.0 * count[e] / n);
    }
    printf("  distinct entries %u\n", s.entries);
    play_check(idx, h, s);

    // deviation from target over flat input areas
    std::vector<float>   tg(n * 3), tl(n * 3);
    std::vector<uint8_t> tn(n);
    for(size_t p = 0; p < n; p++)
    {
        tn[p] = pixel_target(&tc.im.rgb[3 * p], &tg[3 * p]);
        target_lin(&tg[3 * p], &tl[3 * p]);
    }
    {
        double ty = 0, fy = 0, ry = 0;
        for(size_t p = 0; p < n; p++) ty += luma(&tl[3 * p]), fy += luma(g_lin[idx[p]]), ry += luma(g_lin[ref[p]]);
        s.frame_dy = (fy - ty) / n, s.ref_frame_dy = (ry - ty) / n;
        printf("  frame mean entry luminance minus target luminance: fixed %+.5f, float %+.5f\n", s.frame_dy,
               s.ref_frame_dy);
    }
    size_t dl10 = 0, dl15 = 0, dl20 = 0, dc04 = 0, dc06 = 0, dc10 = 0, neutral_px = 0;
    size_t far_by[TONEMAP_COUNT] = {0}, iso_by[TONEMAP_COUNT] = {0};
    size_t far_in[TONEMAP_COUNT][3] = {};
    size_t white = 0, white_off = 0, white_off_flat = 0, black = 0, black_off = 0;
    size_t white_by[TONEMAP_COUNT] = {0}, black_by[TONEMAP_COUNT] = {0}, tint_by[TONEMAP_COUNT] = {0};
    for(int y = 0; y < h; y++)
        for(int x = 0; x < W; x++)
        {
            const size_t   p  = static_cast<size_t>(y) * W + x;
            const uint8_t* in = &tc.im.rgb[3 * p];
            const float* t    = &tg[3 * p];
            if(tn[p])
            {
                neutral_px++;
                if(!g_neutral[idx[p]]) s.tint++, tint_by[idx[p]]++;
            }
            bool flat = true;
            for(int yy = std::max(0, y - 3); flat && yy <= std::min(h - 1, y + 3); yy++)
                for(int xx = std::max(0, x - 3); xx <= std::min(W - 1, x + 3); xx++)
                    if(wdist2(t, &tg[3 * (static_cast<size_t>(yy) * W + xx)]) > FLAT * FLAT)
                    {
                        flat = false;
                        break;
                    }
            if(in[0] == 255 && in[1] == 255 && in[2] == 255)
            {
                const bool off = idx[p] != TONE_PAPER;
                white++, white_off += off, white_off_flat += off && flat;
                if(off) white_by[idx[p]]++;
            }
            if(in[0] == 0 && in[1] == 0 && in[2] == 0)
            {
                black++, black_off += idx[p] != g_black;
                if(idx[p] != g_black) black_by[idx[p]]++;
            }
            if(!flat) continue;
            s.flat++;
            const unsigned e  = idx[p];
            const float    dl = fabsf(g_lab[e][0] - t[0]);
            const float    dc = hypotf(g_lab[e][1] - t[1], g_lab[e][2] - t[2]);
            dl10 += dl > 0.10f, dl15 += dl > 0.15f, dl20 += dl > 0.20f;
            dc04 += dc > 0.04f, dc06 += dc > 0.06f, dc10 += dc > 0.10f;
            if(dl <= FAR_L && dc <= FAR_C) continue;
            s.far++;
            far_by[e]++;
            for(int c = 0; c < 3; c++) far_in[e][c] += in[c];
            bool iso = true;
            for(int yy = std::max(0, y - 1); iso && yy <= std::min(h - 1, y + 1); yy++)
                for(int xx = std::max(0, x - 1); xx <= std::min(W - 1, x + 1); xx++)
                    if((yy != y || xx != x) && idx[static_cast<size_t>(yy) * W + xx] == e)
                    {
                        iso = false;
                        break;
                    }
            if(iso) s.far_iso++, iso_by[e]++;
        }
    printf("  neutral targets %zu of %zu, on an entry with r or R %zu", neutral_px, n, s.tint);
    for(auto& [e, c] : top_entries(tint_by, 3)) printf(", %s %zu", name_of(e), c);
    printf("\n");
    auto pct = [&](size_t v) { return s.flat ? 100.0 * v / s.flat : 0.0; };
    printf("  flat pixels %zu of %zu. Against the target: |dL| > 0.10 %zu (%.2f%%), > 0.15 %zu (%.2f%%), > 0.20 %zu "
           "(%.2f%%). dC > 0.04 %zu (%.2f%%), > 0.06 %zu (%.2f%%), > 0.10 %zu (%.2f%%)\n",
           s.flat, n, dl10, pct(dl10), dl15, pct(dl15), dl20, pct(dl20), dc04, pct(dc04), dc06, pct(dc06), dc10,
           pct(dc10));
    printf("  far (|dL| > %.2f or dC > %.2f) %zu, of them isolated (no 8-neighbour with the same entry) %zu\n",
           static_cast<double>(FAR_L), static_cast<double>(FAR_C), s.far, s.far_iso);
    if(s.far)
    {
        printf("  far by entry, count, isolated, mean input RGB:");
        for(auto& [e, c] : top_entries(far_by, 8))
            printf(" %s %zu %zu (%zu %zu %zu)", name_of(e), c, iso_by[e], far_in[e][0] / c, far_in[e][1] / c,
                   far_in[e][2] / c);
        printf("\n");
    }
    if(white)
        printf("  input white pixels %zu, rendered other than paper %zu, of them in flat areas %zu\n", white,
               white_off, white_off_flat);
    if(black) printf("  input black pixels %zu, rendered other than black %zu\n", black, black_off);
    if(white_off || black_off)
    {
        printf("  off-white as, off-black as (entry L count):");
        for(auto& [e, c] : top_entries(white_by, 3)) printf(" %s %.3f %zu", name_of(e), g_lab[e][0], c);
        printf(",");
        for(auto& [e, c] : top_entries(black_by, 3)) printf(" %s %.3f %zu", name_of(e), g_lab[e][0], c);
        printf("\n");
    }
    s.white = white, s.white_off = white_off, s.white_off_flat = white_off_flat, s.black = black, s.black_off = black_off;

    // ramp columns: the target and the entries as their mean linear light, texture, solid runs
    if(tc.ramp)
    {
        // runs of columns where one entry covers SOLID of the column. reach: the largest |tgt L - entry
        // L| over the run's columns.
        const double SOLID   = 0.98;
        const int    SOLID_W = 8;
        struct solid_run
        {
            int   x0, x1, e;
            float reach;
        };
        std::vector<int> dom(W, -1);
        for(int x = 0; x < W; x++)
        {
            size_t cnt[TONEMAP_COUNT] = {0};
            for(int y = 0; y < h; y++) cnt[idx[static_cast<size_t>(y) * W + x]]++;
            auto top = top_entries(cnt, 1);
            if(static_cast<double>(top[0].second) >= SOLID * h) dom[x] = static_cast<int>(top[0].first);
        }
        std::vector<solid_run> sruns;
        for(int x = 0; x < W;)
        {
            int x1 = x;
            while(x1 < W && dom[x1] == dom[x]) x1++;
            if(dom[x] >= 0)
            {
                solid_run r = {x, x1, dom[x], 0.0f};
                for(int xx = x; xx < x1; xx++) r.reach = std::max(r.reach, fabsf(tg[3 * xx] - g_lab[r.e][0]));
                sruns.push_back(r);
                s.solid_max   = std::max(s.solid_max, x1 - x);
                s.solid_reach = std::max(s.solid_reach, static_cast<double>(r.reach));
            }
            x = x1;
        }

        printf("  x0   input R G B  | tgt L     a      b  | seen L     a      b  |   dY    | chk%%   run  blk | solid "
               "max mean | float seen L   dY    | entries\n");
        float ref_worst_seen = 0;
        float worst_seen = 0;
        int   worst_seen_x = 0, worst_y_x = 0, bands = 0, ratios = 0;
        for(int x0 = 0; x0 < W; x0 += BAND)
        {
            const int x1 = std::min(W, x0 + BAND);
            double    ti[3] = {0, 0, 0}, li[3] = {0, 0, 0}, ri[3] = {0, 0, 0};
            size_t    cnt[TONEMAP_COUNT] = {0}, m = 0;
            for(int y = 0; y < h; y++)
                for(int x = x0; x < x1; x++)
                {
                    const size_t p = static_cast<size_t>(y) * W + x;
                    for(int i = 0; i < 3; i++)
                        ti[i] += tl[3 * p + i], li[i] += g_lin[idx[p]][i], ri[i] += g_lin[ref[p]][i];
                    cnt[idx[p]]++;
                    m++;
                }
            float t_mean[3], e_mean[3], r_mean[3], tgt[3], seen[3], rseen[3];
            for(int i = 0; i < 3; i++)
                t_mean[i] = static_cast<float>(ti[i] / m), e_mean[i] = static_cast<float>(li[i] / m),
                r_mean[i] = static_cast<float>(ri[i] / m);
            oklab_lin(t_mean, tgt);
            oklab_lin(e_mean, seen);
            oklab_lin(r_mean, rseen);
            const float    dY  = luma(e_mean) - luma(t_mean);
            const float    rdY = luma(r_mean) - luma(t_mean);
            ref_worst_seen     = std::max(ref_worst_seen, fabsf(rseen[0] - tgt[0]));
            s.ref_worst_y      = std::max(s.ref_worst_y, static_cast<double>(fabsf(rdY)));
            const uint8_t* c  = &tc.im.rgb[static_cast<size_t>((x0 + x1) / 2) * 3];
            const texture  tx = texture_of(idx, tg, h, x0, x1);
            s.tex.checker += tx.checker, s.tex.run += tx.run, bands++;
            if(tx.ratio >= 0) s.tex.ratio += tx.ratio, ratios++;
            printf("  %3d  %3d %3d %3d  | %5.3f %+6.3f %+6.3f | %5.3f %+6.3f %+6.3f | %+7.4f | %4.1f %5.1f ", x0, c[0],
                   c[1], c[2], tgt[0], tgt[1], tgt[2], seen[0], seen[1], seen[2], dY, 100 * tx.checker, tx.run);
            if(tx.ratio >= 0) printf("%4.2f |", tx.ratio);
            else printf("   - |");
            int    smax = 0, sn = 0;
            double ssum = 0;
            for(auto& r : sruns)
                if(r.x1 > x0 && r.x0 < x1) smax = std::max(smax, r.x1 - r.x0), ssum += r.x1 - r.x0, sn++;
            printf("     %3d %4.1f |", smax, sn ? ssum / sn : 0.0);
            printf("       %5.3f %+7.4f |", rseen[0], rdY);
            for(auto& [e, cn] : top_entries(cnt, 3)) printf(" %s %.0f%%", name_of(e), 100.0 * cn / m);
            printf("\n");
            if(fabsf(seen[0] - tgt[0]) > worst_seen) worst_seen = fabsf(seen[0] - tgt[0]), worst_seen_x = x0;
            if(fabs(dY) > s.worst_y) s.worst_y = fabs(dY), worst_y_x = x0;
            s.worst_seen_c = std::max(s.worst_seen_c, static_cast<double>(hypotf(seen[1] - tgt[1], seen[2] - tgt[2])));
        }
        s.worst_seen     = worst_seen;
        s.ref_worst_seen = ref_worst_seen;
        printf("  float dither: largest band |seen L - tgt L| %.4f, |dY| %.4f\n", s.ref_worst_seen, s.ref_worst_y);
        printf("  largest band |seen L - tgt L| %.4f at x0 %d, |dY| %.4f at x0 %d, a, b distance of seen from tgt "
               "%.4f\n",
               static_cast<double>(worst_seen), worst_seen_x, s.worst_y, worst_y_x, s.worst_seen_c);
        s.tex.checker /= bands, s.tex.run /= bands;
        s.tex.ratio = ratios ? s.tex.ratio / ratios : -1;
        printf("  band means: checker %.1f%%, run %.1f px, block ratio %.2f over %d of %d bands\n",
               100 * s.tex.checker, s.tex.run, s.tex.ratio, ratios, bands);

        printf("  solid runs (one entry on >= %.0f%% of each column, >= %d px):", 100 * SOLID, SOLID_W);
        int runs = 0;
        for(auto& r : sruns)
        {
            if(r.x1 - r.x0 < SOLID_W) continue;
            printf("%s x %d-%d (%d px) %s L %.4f, tgt L %.4f to %.4f", runs ? "," : "", r.x0, r.x1 - 1, r.x1 - r.x0,
                   name_of(static_cast<unsigned>(r.e)), g_lab[r.e][0], tg[3 * r.x0], tg[3 * (r.x1 - 1)]);
            runs++;
            s.solid_px += r.x1 - r.x0;
        }
        printf("%s\n", runs ? "" : " none");
        printf("  all solid runs: %zu, longest %d px, largest reach |tgt L - entry L| %.4f\n", sruns.size(),
               s.solid_max, s.solid_reach);
    }
    else
    {
        s.tex = texture_of(idx, tg, h, 0, W);
        printf("  texture: checker %.1f%%, run %.1f px, block ratio %.2f\n", 100 * s.tex.checker, s.tex.run,
               s.tex.ratio);
    }
    sum.push_back(s);
}

// (a) white to black, 960x200. The crop centres on the widest L gap between neutral entries.
test_case gray_ramp()
{
    test_case tc;
    tc.name = "a_gray_ramp";
    tc.im   = blank(200);
    tc.ramp = true;
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++)
        {
            const int v = 255 - ramp_at(x);
            put(tc.im, x, y, v, v, v);
        }
    tc.crop_x = gray_x_of_l(widest_gap_mid(1.0f)) - CROP_W / 2;
    tc.crop_y = 20;
    tc.dark_x = gray_x_of_l(0.42f) - DARK_W / 2;
    tc.dark_y = 40;
    return tc;
}

// Dark ramp: gray DARK_MAX at x 0, 0 at x W - 1.
const int DARK_MAX = 110;
int       dark_at(int x)
{
    return DARK_MAX - (x * DARK_MAX + (W - 1) / 2) / (W - 1);
}

// Column of the dark ramp whose gray target has Oklab lightness L.
int dark_x_of_l(float l)
{
    int bx = 0;
    for(int x = 1; x < W; x++)
        if(fabsf(gray_target_l(dark_at(x)) - l) < fabsf(gray_target_l(dark_at(bx)) - l)) bx = x;
    return bx;
}

// (a2) sRGB gray DARK_MAX to 0 across x, 960x200: target L 0.54 to 0. The crop centres on L 0.42,
// the dark crop on the widest L gap between neutral entries at or below L 0.45.
test_case dark_gray_ramp()
{
    test_case tc;
    tc.name = "a2_dark_gray_ramp";
    tc.im   = blank(200);
    tc.ramp = true;
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, dark_at(x), dark_at(x), dark_at(x));
    tc.crop_x = dark_x_of_l(0.42f) - CROP_W / 2;
    tc.crop_y = 20;
    tc.dark_x = dark_x_of_l(widest_gap_mid(0.45f)) - DARK_W / 2;
    tc.dark_y = 40;
    return tc;
}

// (b3) (DARK_MAX, 0, 0) to black, 960x200.
test_case dark_red_ramp()
{
    test_case tc;
    tc.name = "b3_dark_red_ramp";
    tc.im   = blank(200);
    tc.ramp = true;
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, dark_at(x), 0, 0);
    tc.crop_x = W / 2 - CROP_W / 2;
    tc.crop_y = 20;
    tc.dark_x = W / 2 - DARK_W / 2;
    tc.dark_y = 40;
    return tc;
}

// (b1) white to (255, 0, 0), 960x200: G and B fall across x.
test_case red_sat_ramp()
{
    test_case tc;
    tc.name = "b1_red_sat_ramp";
    tc.im   = blank(200);
    tc.ramp = true;
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, 255, 255 - ramp_at(x), 255 - ramp_at(x));
    tc.crop_x = 600;
    tc.crop_y = 20;
    tc.dark_x = 780;
    tc.dark_y = 40;
    return tc;
}

// (b2) (255, 0, 0) to black, 960x200.
test_case red_dark_ramp()
{
    test_case tc;
    tc.name = "b2_red_dark_ramp";
    tc.im   = blank(200);
    tc.ramp = true;
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, 255 - ramp_at(x), 0, 0);
    tc.crop_x = 360;
    tc.crop_y = 20;
    tc.dark_x = 640;
    tc.dark_y = 40;
    return tc;
}

// (c1) 960x672: R falls from 255 to 0 across x, and G = B lie below R by 0 to 255 down y, at least
// 0. Paper where the two steps add past 255.
test_case tri_axes()
{
    test_case tc;
    tc.name = "c1_tri_axes";
    tc.im   = blank(TONE_H);
    for(int y = 0; y < tc.im.h; y++)
    {
        const int dy = (y * 255 + (TONE_H - 1) / 2) / (TONE_H - 1);
        for(int x = 0; x < W; x++)
        {
            const int dx = ramp_at(x);
            if(dx + dy > 255) continue;
            const int r  = 255 - dx;
            const int gb = std::max(0, r - dy);
            put(tc.im, x, y, r, gb, gb);
        }
    }
    tc.crop_x = 400;
    tc.crop_y = 100;
    tc.dark_x = 700;
    tc.dark_y = 0;
    return tc;
}

// (c2) 960x672: a wedge with white at the apex on top, black at the bottom left and (255, 0, 0) at
// the bottom right, each pixel the sRGB mix of the three by its weights. Paper outside.
test_case tri_show()
{
    const int TRI_BASE_W = 700, TRI_BASE_Y = 640, TRI_HEIGHT = 606;
    test_case tc;
    tc.name           = "c2_tri_show";
    tc.im             = blank(TONE_H);
    const int apex_y  = TRI_BASE_Y - TRI_HEIGHT;
    const int left_x  = (W - TRI_BASE_W) / 2;
    for(int y = apex_y; y < TRI_BASE_Y; y++)
    {
        const int t    = y - apex_y;
        const int half = TRI_BASE_W * t / (2 * TRI_HEIGHT);
        const int cx   = left_x + TRI_BASE_W / 2;
        for(int x = cx - half; x <= cx + half; x++)
        {
            const int w_white = 255 - t * 255 / TRI_HEIGHT;
            const int span    = 2 * half;
            const int w_red   = span > 0 ? (255 - w_white) * (x - (cx - half)) / span : 0;
            put(tc.im, x, y, w_white + w_red, w_white, w_white);
        }
    }
    tc.crop_x = 560;
    tc.crop_y = 460;
    tc.dark_x = 200;
    tc.dark_y = 500;
    return tc;
}

#if HAVE_FREETYPE
// Text as render.cpp draw_text() draws it: FreeType's default render, coverage into blend().
struct ft_text
{
    FT_Library lib  = nullptr;
    FT_Face    face = nullptr;
    bool       open(const char* path) { return !FT_Init_FreeType(&lib) && !FT_New_Face(lib, path, 0, &face); }
    ~ft_text()
    {
        if(face) FT_Done_Face(face);
        if(lib) FT_Done_FreeType(lib);
    }
    void draw(image& im, int size, int x, int base, const char* s, const uint8_t c[3])
    {
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
        FT_Pos pen = static_cast<FT_Pos>(x) * 64;
        for(const char* p = s; *p; p++)
        {
            if(FT_Load_Char(face, static_cast<unsigned char>(*p), FT_LOAD_RENDER)) continue;
            FT_GlyphSlot     g  = face->glyph;
            const FT_Bitmap& bm = g->bitmap;
            int              gx = static_cast<int>(pen >> 6) + g->bitmap_left, gy = base - g->bitmap_top;
            for(unsigned yy = 0; yy < bm.rows; yy++)
            {
                const uint8_t* row = bm.buffer + static_cast<long>(yy) * bm.pitch;
                for(unsigned xx = 0; xx < bm.width; xx++)
                {
                    int cov = bm.pixel_mode == FT_PIXEL_MODE_MONO ? (((row[xx >> 3] >> (7 - (xx & 7))) & 1) ? 255 : 0)
                                                                  : row[xx];
                    blend(im, gx + static_cast<int>(xx), gy + static_cast<int>(yy), c, cov);
                }
            }
            pen += g->advance.x;
        }
    }
};
#endif

// (d) FreeType text in black at 10 to 64 px, coloured lines, white on black, black on pink, then
// vertical strokes 1 to 4 px wide at gray 0, 64, 128 and 192.
test_case text_block()
{
    test_case tc;
    tc.name                 = "d_text";
    tc.im                   = blank(TONE_H);
    const uint8_t black[3]  = {0, 0, 0};
    const uint8_t white[3]  = {255, 255, 255};
    const uint8_t pink[3]   = {255, 204, 204};
    const char*   pangram   = "The quick brown fox jumps over the lazy dog 0123456789";
#if HAVE_FREETYPE
    const char* path = getenv("TONES_FONT") ? getenv("TONES_FONT") : DEFAULT_FONT;
    ft_text     ft;
    if(ft.open(path))
    {
        int y = 2;
        for(int sz : {10, 12, 14, 16, 20, 24, 32, 48, 64})
        {
            char line[128];
            snprintf(line, sizeof(line), "%d px %s", sz, pangram);
            y += sz;
            ft.draw(tc.im, sz, 8, y, line, black);
            y += sz / 3 + 2;
        }
        struct
        {
            uint8_t     c[3];
            const char* label;
        } colours[] = {{{255, 0, 0}, "#ff0000 red text"},
                       {{204, 0, 0}, "#cc0000 dark red text"},
                       {{128, 128, 128}, "#808080 gray text"},
                       {{0, 0, 238}, "#0000ee link blue text"}};
        for(auto& c : colours)
        {
            char line[128];
            snprintf(line, sizeof(line), "16 px %s: %s", c.label, pangram);
            y += 18;
            ft.draw(tc.im, 16, 8, y, line, c.c);
        }
        y += 6;
        const uint8_t red[3] = {255, 0, 0};
        ft.draw(tc.im, 32, 8, y + 32, "32 px #ff0000 Headline Red", red);
        y += 42;
        tc.dark_y = y - 56;
        fill_rect(tc.im, 0, y, W, y + 28, black);
        ft.draw(tc.im, 20, 8, y + 21, "20 px white on black: The quick brown fox jumps", white);
        y += 30;
        fill_rect(tc.im, 0, y, W, y + 24, pink);
        ft.draw(tc.im, 16, 8, y + 18, "16 px black on #ffcccc: The quick brown fox jumps over the lazy dog", black);
        printf("text face %s, text ends at row %d\n", path, y + 24);
    }
    else
        printf("text face %s did not open, text block skipped\n", path);
#else
    (void)black, (void)white, (void)pink, (void)pangram;
    printf("built without FreeType, text block skipped\n");
#endif
    const int levels[4] = {0, 64, 128, 192};
    for(int w = 1; w <= 4; w++)
    {
        const int y0 = TONE_H - (5 - w) * 22;
        for(int sec = 0; sec < 4; sec++)
        {
            const int     sx  = sec * (W / 4) + 8;
            const uint8_t v   = static_cast<uint8_t>(levels[sec]);
            const uint8_t c[3] = {v, v, v};
            for(int x = sx; x < (sec + 1) * (W / 4) - 8; x++)
                if((x - sx) % (2 * w) < w)
                    for(int y = y0; y < y0 + 18; y++) blend(tc.im, x, y, c, 255);
        }
    }
    tc.crop_x = 0;
    tc.crop_y = 0;
    return tc;
}

// (e) a PNG, 8-bit gray plus alpha, as RGB: R is 255 minus the gray, G and B
// are R minus the alpha, at least 0.
bool tone_image(const char* path, test_case& tc)
{
    unsigned char* png = nullptr;
    size_t         len = 0;
    if(lodepng_load_file(&png, &len, path))
    {
        fprintf(stderr, "%s: cannot read\n", path);
        return false;
    }
    LodePNGState st;
    lodepng_state_init(&st);
    unsigned       w = 0, h = 0;
    unsigned char* ga  = nullptr;
    unsigned       err = lodepng_inspect(&w, &h, &st, png, len);
    bool           ok  = !err && st.info_png.color.colortype == LCT_GREY_ALPHA && st.info_png.color.bitdepth == 8 &&
                w == static_cast<unsigned>(W) && h >= 1 && h <= static_cast<unsigned>(TONE_H);
    if(ok)
    {
        st.info_raw.colortype = LCT_GREY_ALPHA;
        st.info_raw.bitdepth  = 8;
        ok                    = !lodepng_decode(&ga, &w, &h, &st, png, len);
    }
    lodepng_state_cleanup(&st);
    free(png);
    if(!ok)
    {
        fprintf(stderr, "%s: not a 960 wide 8-bit gray plus alpha PNG\n", path);
        free(ga);
        return false;
    }
    std::string name = path;
    name             = name.substr(name.find_last_of('/') + 1);
    name             = "e_image_" + name.substr(0, name.find_last_of('.'));
    tc.name          = name;
    tc.im            = blank(static_cast<int>(h));
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++)
        {
            const uint8_t* p  = &ga[(static_cast<size_t>(y) * W + x) * 2];
            const int      r  = 255 - p[0];
            const int      gb = std::max(0, r - p[1]);
            put(tc.im, x, y, r, gb, gb);
        }
    free(ga);
    tc.crop_x = W / 2 - CROP_W / 2;
    tc.crop_y = tc.im.h / 2 - CROP_H / 2;
    tc.dark_x = 620; // the dark lower right of image 01
    tc.dark_y = 460;
    return true;
}

// Neighbouring neutral entries: their L, their linear light and the gap in each. The carried
// error of a neutral pixel stays inside its pair, so the widest linear gap bounds it.
void neutral_pairs()
{
    const std::vector<unsigned> nt = neutrals();
    printf("\n== neutral pairs, lightest first: L and linear light of each, and the gaps\n"
           "  lighter            L      lin    darker             L      lin    dL      dlin\n");
    float wl = 0, wy = 0;
    for(size_t i = 0; i + 1 < nt.size(); i++)
    {
        const unsigned a = nt[i], b = nt[i + 1];
        const float    dl = g_lab[a][0] - g_lab[b][0], dy = g_lin[a][1] - g_lin[b][1];
        wl = std::max(wl, dl), wy = std::max(wy, dy);
        printf("  %-16s %6.4f %6.4f  %-16s %6.4f %6.4f  %6.4f  %6.4f\n", name_of(a), g_lab[a][0], g_lin[a][1],
               name_of(b), g_lab[b][0], g_lin[b][1], dl, dy);
    }
    printf("  %zu neutral entries, widest gap %.4f in L, %.4f in linear light\n", nt.size(), wl, wy);
}

// tones_draw() on a panel without the tables: the driver model refuses the session, and the frame
// holds every entry once at the start of row 0. Reads back the stock colour each entry got.
void stock_check()
{
    std::vector<uint8_t> frame(static_cast<size_t>(TONE_W) * TONE_H, TONE_PAPER);
    for(unsigned e = 0; e < N; e++) frame[e] = static_cast<uint8_t>(e);
    g_stub           = epd_stub_log();
    g_stub.no_tables = true;
    tone_draw_stats ds;
    const bool      ok = tones_draw(frame.data(), &ds);
    unsigned        n[4] = {0, 0, 0, 0};
    std::string     list[4];
    for(unsigned e = 0; e < N; e++)
    {
        const uint8_t m    = static_cast<uint8_t>(0x80u >> (e & 7));
        const int     code = ((epd_plane_red()[e >> 3] & m) ? 2 : 0) | ((epd_plane_bw()[e >> 3] & m) ? 1 : 0);
        n[code]++;
        char item[64];
        snprintf(item, sizeof(item), " %s (L %.3f C %.3f)", name_of(e), g_lab[e][0], hypotf(g_lab[e][1], g_lab[e][2]));
        list[code] += item;
    }
    printf("\n== stock draw: %s, %u refresh, mode %s, opens refused. %u white, %u black, %u red, %u both bits\n",
           ok ? "ok" : "failed", g_stub.refreshes, g_stub.refresh_mode == EPD_SLOW ? "slow" : "fast", n[0], n[1], n[2],
           n[3]);
    printf("  black:%s\n  red:%s\n", list[1].c_str(), list[2].c_str());
}

const char* SUMMARY_HEAD = "test\tentries\tpulls1\tpulls2\tsends\tplay_bad\tfar\ttint\twhite\twhite_off\twhite_off_flat\t"
                           "black\tblack_off\tworst_seen\tworst_c\tworst_y\tsolid_px\tsolid_max\tsolid_reach\tchecker\t"
                           "run\tratio\tms\tref_differ\tref_worst_seen\tref_worst_y\tframe_dy\tref_frame_dy\tref_ms";

void write_summary(const std::string& path, const std::vector<summary_row>& sum)
{
    FILE* f = fopen(path.c_str(), "w");
    if(!f) return;
    fprintf(f, "%s\n", SUMMARY_HEAD);
    for(auto& s : sum)
        fprintf(f, "%s\t%u\t%u\t%u\t%u\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%.4f\t%.4f\t%.4f\t%d\t%d\t%.4f\t%.2f\t%.2f\t"
                   "%.2f\t%.1f\t%.4f\t%.4f\t%.4f\t%.5f\t%.5f\t%.1f\n",
                s.name.c_str(), s.entries, s.pulls1, s.pulls2, s.sends, s.play_bad, s.far, s.tint, s.white, s.white_off,
                s.white_off_flat, s.black, s.black_off, s.worst_seen, s.worst_seen_c, s.worst_y, s.solid_px, s.solid_max,
                s.solid_reach, s.checker_pct(), s.tex.run, s.tex.ratio, s.ms, s.ref_differ, s.ref_worst_seen,
                s.ref_worst_y, s.frame_dy, s.ref_frame_dy, s.ref_ms);
    fclose(f);
}

std::vector<std::vector<std::string>> read_tsv(const std::string& path)
{
    std::vector<std::vector<std::string>> rows;
    FILE*                                 f = fopen(path.c_str(), "r");
    if(!f) return rows;
    char line[1024];
    while(fgets(line, sizeof(line), f))
    {
        std::vector<std::string> row;
        std::string              cell;
        for(const char* p = line; *p && *p != '\n'; p++)
        {
            if(*p == '\t') row.push_back(cell), cell.clear();
            else cell += *p;
        }
        row.push_back(cell);
        rows.push_back(row);
    }
    fclose(f);
    return rows;
}

// --compare: per test, the summary rows of every config side by side, and SWEEPDIR/<test>_compare.png
// with every config's dark crop under its name, three across.
int compare(const std::string& dir, const std::vector<std::string>& cfgs)
{
    std::vector<std::vector<std::vector<std::string>>> tabs;
    for(auto& c : cfgs)
    {
        tabs.push_back(read_tsv(dir + "/" + c + "/summary.tsv"));
        if(tabs.back().size() < 2)
        {
            fprintf(stderr, "%s/%s/summary.tsv: missing or empty\n", dir.c_str(), c.c_str());
            return 1;
        }
    }
#if HAVE_FREETYPE
    const char* path = getenv("TONES_FONT") ? getenv("TONES_FONT") : DEFAULT_FONT;
    ft_text     ft;
    const bool  font = ft.open(path);
#endif
    const int TW = DARK_W * CROP_S, TH = DARK_H * CROP_S, LABEL = 30, GAP = 8;
    const int cols = std::min<int>(3, static_cast<int>(cfgs.size()));
    const int rows = (static_cast<int>(cfgs.size()) + cols - 1) / cols;
    const int mw = cols * TW + (cols + 1) * GAP, mh = rows * (TH + LABEL) + (rows + 1) * GAP;
    const auto& head = tabs[0][0];
    for(size_t t = 1; t < tabs[0].size(); t++)
    {
        const std::string& test = tabs[0][t][0];
        printf("\n== %s\n  %-24s", test.c_str(), "config");
        for(size_t k = 1; k < head.size(); k++) printf(" %*s", static_cast<int>(std::max<size_t>(head[k].size(), 6)), head[k].c_str());
        printf("\n");
        std::vector<uint8_t> m(static_cast<size_t>(mw) * mh * 3, 160);
        for(size_t c = 0; c < cfgs.size(); c++)
        {
            for(auto& r : tabs[c])
            {
                if(r[0] != test) continue;
                printf("  %-24s", cfgs[c].c_str());
                for(size_t k = 1; k < r.size() && k < head.size(); k++)
                    printf(" %*s", static_cast<int>(std::max<size_t>(head[k].size(), 6)), r[k].c_str());
                printf("\n");
            }
            const int ox = GAP + static_cast<int>(c) % cols * (TW + GAP);
            const int oy = GAP + static_cast<int>(c) / cols * (TH + LABEL + GAP);
            image     lab = blank(LABEL);
#if HAVE_FREETYPE
            const uint8_t label_c[3] = {0, 0, 0};
            if(font) ft.draw(lab, 20, 6, 22, cfgs[c].c_str(), label_c);
#endif
            for(int y = 0; y < LABEL; y++) memcpy(&m[(static_cast<size_t>(oy + y) * mw + ox) * 3], px(lab, 0, y), TW * 3);
            unsigned char* png = nullptr;
            unsigned       w = 0, h = 0;
            const std::string crop = dir + "/" + cfgs[c] + "/" + test + "_dark4x.png";
            if(lodepng_decode24_file(&png, &w, &h, crop.c_str()) || w != static_cast<unsigned>(TW) ||
               h != static_cast<unsigned>(TH))
                fprintf(stderr, "%s: missing or not %dx%d\n", crop.c_str(), TW, TH);
            else
                for(int y = 0; y < TH; y++)
                    memcpy(&m[(static_cast<size_t>(oy + LABEL + y) * mw + ox) * 3], &png[static_cast<size_t>(y) * TW * 3], TW * 3);
            free(png);
        }
        save_png(dir + "/" + test + "_compare.png", m.data(), mw, mh);
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc >= 4 && strcmp(argv[1], "--compare") == 0)
        return compare(argv[2], std::vector<std::string>(argv + 3, argv + argc));
    if(argc < 2)
    {
        fprintf(stderr, "usage: %s OUTDIR [TONE_IMAGE.png ...]\n       %s --compare SWEEPDIR CONFIG ...\n", argv[0],
                argv[0]);
        return 2;
    }
    const std::string outdir = argv[1];
    const auto        t0     = std::chrono::steady_clock::now();
    if(!tones_init() || !analysis_init()) return 1;
    printf("init %.1f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    printf("tones_solid_gray: 174 -> %d, 200 -> %d, 0 -> %d, 255 -> %d\n", tones_solid_gray(174), tones_solid_gray(200),
           tones_solid_gray(0), tones_solid_gray(255));
    {
        int differ = 0;
        for(int g = 0; g < 256; g++)
            if(tones_solid_gray(g) != tones_float_ref_solid_gray(g))
            {
                if(differ < 8) printf("  gray %d: fixed %d, float %d\n", g, tones_solid_gray(g), tones_float_ref_solid_gray(g));
                differ++;
            }
        printf("tones_solid_gray against the float targets: %d of 256 gray bytes differ\n", differ);
    }

    std::vector<summary_row> sum;
    run(gray_ramp(), outdir, sum);
    run(dark_gray_ramp(), outdir, sum);
    run(red_sat_ramp(), outdir, sum);
    run(red_dark_ramp(), outdir, sum);
    run(dark_red_ramp(), outdir, sum);
    run(tri_axes(), outdir, sum);
    run(tri_show(), outdir, sum);
    run(text_block(), outdir, sum);
    for(int i = 2; i < argc; i++)
    {
        test_case tc;
        if(tone_image(argv[i], tc)) run(tc, outdir, sum);
    }
    neutral_pairs();
    // last: the driver model then refuses every session, and tones.cpp keeps to the stock draw
    stock_check();

    printf("\n== summary\n  test                 entries  pulls  sends  play    flat px    far  far iso   tint  "
           "white off  black off  worst seen L  worst dY  checker   run  ratio  solid max\n");
    for(auto& s : sum)
        printf("  %-20s %7u %3u+%-3u %5u  %4s %10zu %6zu %8zu %6zu %10zu %10zu %13.4f %9.4f %7.1f%% %5.1f %6.2f %10d\n",
               s.name.c_str(), s.entries, s.pulls1, s.pulls2, s.sends, s.play_bad ? "FAIL" : "ok", s.flat, s.far,
               s.far_iso, s.tint, s.white_off, s.black_off, s.worst_seen, s.worst_y, s.checker_pct(), s.tex.run,
               s.tex.ratio, s.solid_max);
    printf("\n== fixed point against float: share of pixels on another entry, the band checks of the ramps, the frame "
           "mean luminance error, host ms\n"
           "  test                 differ  worst seen L fixed  float  worst dY fixed   float  frame dY fixed    float"
           "      ms fixed  float\n");
    for(auto& s : sum)
        printf("  %-20s %6.2f%%  %18.4f %6.4f  %14.4f %7.4f  %14.5f %8.5f  %12.1f %6.1f\n", s.name.c_str(),
               100 * s.ref_differ, s.worst_seen, s.ref_worst_seen, s.worst_y, s.ref_worst_y, s.frame_dy, s.ref_frame_dy,
               s.ms, s.ref_ms);
    write_summary(outdir + "/summary.tsv", sum);
    return 0;
}
