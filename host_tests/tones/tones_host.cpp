// Host test of the tone dither and the frame play of main/tones.cpp. Run from this directory: make test.
//
// Each test image runs through tone_dither::row, some of them with a photo mask, and three checks
// run on the entry frame it gives. An image with a mask runs in the photo modes triangle and
// nearest (tones.h), and the seam image in diffuse too:
// 1. The play check. The frame, with paper rows below it to TONE_H, plays through tones_draw() into
//    the driver model of epd_stub.cpp. tone_plan_build() makes a plan, the code of every pixel at
//    every pull equals tone_plan_code() of its entry after the COG's plane swap, the pulls of each
//    phase equal the plan in the model and in the stats of tones_draw(), the first pull of each
//    session sends the planes, each session opens on the right table and unit in phase order,
//    tones_draw() returns true, and at the end the session is closed and the planes are clear.
// 2. The neutral check. No pixel whose target has no chroma after the squeeze takes an entry whose
//    pattern has r or R.
// 3. The white and black check. Every input pixel 255 255 255 takes the paper entry, and every input
//    pixel 0 0 0 takes the black entry.
// 4. The mode check. The seam image in diffuse gives the frame of the same image without a mask,
//    and its pixels outside the mask are the same in triangle and nearest.
// 5. The nearest mix of tone_photo.h on a made up graph of three entries (near_toy()).
// Prints one line per run. Exit status 1 when a check fails.
// TONES_FONT in the environment names the face of the text image. Without FreeType, or when the face
// does not open, the text image holds only its strokes.

#include <algorithm>
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
#include "tone_photo.h"
#include "tonemap_squeeze.h"
#include "tones.h"

#if HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace
{
const int      W = TONE_W;
const unsigned N = static_cast<unsigned>(TONEMAP_COUNT);

// NEUTRAL_C of main/tones_float_ref.inc. A target with less a, b chroma after the squeeze is neutral.
const float NEUTRAL_C = 1e-3f;

#if HAVE_FREETYPE
const char* DEFAULT_FONT = "../../data/fonts/sans.ttf"; // the font pack's sans, from host_tests/tones
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

// ---------------------------------------------------------------- the target, as the float reference decides it

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

void oklab_rgb(uint8_t r, uint8_t g, uint8_t b, float out[3])
{
    const float c[3] = {g_byte_lin[r], g_byte_lin[g], g_byte_lin[b]};
    oklab_lin(c, out);
}

int entry_of(const char* pattern)
{
    for(unsigned e = 0; e < N; e++)
        if(strcmp(k_tonemap[e].pattern, pattern) == 0) return static_cast<int>(e);
    return -1;
}

bool target_init()
{
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        g_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for(unsigned e = 0; e < N; e++) g_neutral[e] = strpbrk(k_tonemap[e].pattern, "rR") == nullptr;
    const int eb = entry_of(TONEMAP_BLACK_PATTERN);
    if(eb < 0) return false;
    g_black = static_cast<unsigned>(eb);
    tonemap_squeeze_begin(oklab_rgb);
    return true;
}

// The decision of make_target() in main/tones_float_ref.inc. The pixel's Oklab through
// tonemap_squeeze() is neutral when its a, b chroma is below NEUTRAL_C.
bool neutral_target(const uint8_t* c)
{
    float t[3];
    oklab_rgb(c[0], c[1], c[2], t);
    tonemap_squeeze(t);
    return t[1] * t[1] + t[2] * t[2] < NEUTRAL_C * NEUTRAL_C;
}

// ---------------------------------------------------------------- the checks

struct test_case
{
    std::string          name;
    image                im;
    std::vector<uint8_t> mask; // W x h, not 0 for a photo pixel, or empty for none
};

// Plays the entry frame (h rows, paper below) through tones_draw() into the driver model. Returns
// true when the play matches the plan, else prints what differs.
bool play_ok(const std::vector<uint8_t>& idx, int h, const std::string& name)
{
    std::vector<uint8_t> frame(static_cast<size_t>(TONE_W) * TONE_H, TONE_PAPER);
    memcpy(frame.data(), idx.data(), static_cast<size_t>(W) * h);
    uint32_t count[TONEMAP_COUNT] = {0};
    for(uint8_t e : frame) count[e]++;
    tone_plan  plan;
    const bool built = tone_plan_build(count, &plan);
    g_stub           = epd_stub_log();
    g_stub.frame     = frame.data();
    g_stub.plan      = plan;
    g_stub.count     = count;
    tone_draw_stats ds;
    const bool      ok    = tones_draw(frame.data(), &ds);
    bool            clear = true;
    for(size_t i = 0; i < EPD_PLANE_BYTES; i++) clear = clear && !epd_plane_bw()[i] && !epd_plane_red()[i];
    const epd_stub_log& g    = g_stub;
    const bool          good = built && ok && clear && !epd_tone_is_open() && g.mismatches == 0 && g.first_kept == 0 &&
                      g.bad_open == 0 && g.pulls[TONE_PHASE_1] == plan.len[TONE_PHASE_1] &&
                      g.pulls[TONE_PHASE_2] == plan.len[TONE_PHASE_2] && ds.pulls1 == plan.len[TONE_PHASE_1] &&
                      ds.pulls2 == plan.len[TONE_PHASE_2];
    if(!good)
        printf("FAIL  %s play: plan %s, %u + %u pulls, played %u + %u, tones_draw() counted %u + %u, %u opens, "
               "%u closes, mismatched pixel pulls %zu, first pulls kept %u, bad opens %u, tones_draw() %s, "
               "session %s, planes %s\n",
               name.c_str(), built ? "built" : "not built", plan.len[TONE_PHASE_1], plan.len[TONE_PHASE_2],
               g.pulls[TONE_PHASE_1], g.pulls[TONE_PHASE_2], static_cast<unsigned>(ds.pulls1),
               static_cast<unsigned>(ds.pulls2), g.opens, g.closes, g.mismatches, g.first_kept, g.bad_open,
               ok ? "ok" : "failed", epd_tone_is_open() ? "open" : "closed", clear ? "clear" : "not clear");
    return good;
}

// Runs the checks on the frame of tc in the photo mode tones_photo_mode() gives, and keeps the
// frame in *frame when frame is not null.
bool run(const test_case& tc, std::vector<uint8_t>* frame = nullptr)
{
    const int    h = tc.im.h;
    const size_t n = static_cast<size_t>(W) * h;

    std::vector<uint8_t> idx(n);
    tone_dither          q;
    q.alloc(W);
    for(int y = 0; y < h; y++)
        q.row(&tc.im.rgb[static_cast<size_t>(y) * W * 3], y, &idx[static_cast<size_t>(y) * W],
              tc.mask.empty() ? nullptr : &tc.mask[static_cast<size_t>(y) * W]);
    q.release();

    const bool play = play_ok(idx, h, tc.name);
    size_t     tint = 0, white = 0, white_off = 0, black = 0, black_off = 0;
    for(size_t p = 0; p < n; p++)
    {
        const uint8_t* in = &tc.im.rgb[3 * p];
        if(!g_neutral[idx[p]] && neutral_target(in)) tint++;
        if(in[0] == 255 && in[1] == 255 && in[2] == 255) white++, white_off += idx[p] != TONE_PAPER;
        if(in[0] == 0 && in[1] == 0 && in[2] == 0) black++, black_off += idx[p] != g_black;
    }
    const bool good = play && tint == 0 && white_off == 0 && black_off == 0;
    printf("%s  %-28s play %s, neutral targets on an entry with r or R %zu, white pixels not on paper %zu of %zu, "
           "black pixels not on black %zu of %zu\n",
           good ? "pass" : "FAIL", tc.name.c_str(), play ? "ok" : "FAIL", tint, white_off, white, black_off, black);
    if(frame) frame->swap(idx);
    return good;
}

// ---------------------------------------------------------------- the test images

// Ramp image column x to 0..255.
int ramp_at(int x)
{
    return (x * 255 + (W - 1) / 2) / (W - 1);
}

// White to black across x, 960x200.
test_case gray_ramp()
{
    test_case tc;
    tc.name = "a_gray_ramp";
    tc.im   = blank(200);
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++)
        {
            const int v = 255 - ramp_at(x);
            put(tc.im, x, y, v, v, v);
        }
    return tc;
}

// The dark ramp is gray DARK_MAX at x 0 and 0 at x W - 1.
const int DARK_MAX = 110;
int       dark_at(int x)
{
    return DARK_MAX - (x * DARK_MAX + (W - 1) / 2) / (W - 1);
}

// sRGB gray DARK_MAX to 0 across x, 960x200.
test_case dark_gray_ramp()
{
    test_case tc;
    tc.name = "a2_dark_gray_ramp";
    tc.im   = blank(200);
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, dark_at(x), dark_at(x), dark_at(x));
    return tc;
}

// (DARK_MAX, 0, 0) to black, 960x200.
test_case dark_red_ramp()
{
    test_case tc;
    tc.name = "b3_dark_red_ramp";
    tc.im   = blank(200);
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, dark_at(x), 0, 0);
    return tc;
}

// White to (255, 0, 0), 960x200. G and B fall across x.
test_case red_sat_ramp()
{
    test_case tc;
    tc.name = "b1_red_sat_ramp";
    tc.im   = blank(200);
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, 255, 255 - ramp_at(x), 255 - ramp_at(x));
    return tc;
}

// (255, 0, 0) to black, 960x200.
test_case red_dark_ramp()
{
    test_case tc;
    tc.name = "b2_red_dark_ramp";
    tc.im   = blank(200);
    for(int y = 0; y < tc.im.h; y++)
        for(int x = 0; x < W; x++) put(tc.im, x, y, 255 - ramp_at(x), 0, 0);
    return tc;
}

// 960x672. R falls from 255 to 0 across x, and G = B lie below R by 0 to 255 down y, at least 0.
// Paper where the two steps add past 255.
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
    return tc;
}

// 960x672, a wedge with white at the apex on top, black at the bottom left and (255, 0, 0) at the
// bottom right, each pixel the sRGB mix of the three by its weights. Paper outside.
test_case tri_show()
{
    const int TRI_BASE_W = 700, TRI_BASE_Y = 640, TRI_HEIGHT = 606;
    test_case tc;
    tc.name          = "c2_tri_show";
    tc.im            = blank(TONE_H);
    const int apex_y = TRI_BASE_Y - TRI_HEIGHT;
    const int left_x = (W - TRI_BASE_W) / 2;
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
    return tc;
}

// The same image with every pixel a photo pixel.
test_case as_photo(test_case tc, const char* name)
{
    tc.name = name;
    tc.mask.assign(static_cast<size_t>(W) * tc.im.h, 1);
    return tc;
}

uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 960x672, a made up photo: a sky from pale pink to deep red over a dark ground, a soft radial
// vignette, a gray ball lit from the top left, a red cloth, a dark gray ramp along the bottom and
// noise of up to 6 sRGB steps.
test_case scene()
{
    test_case tc;
    tc.name = "e_scene";
    tc.im   = blank(TONE_H);
    for(int y = 0; y < TONE_H; y++)
        for(int x = 0; x < W; x++)
        {
            float c[3];
            if(y < 400)
            {
                const float t = y / 400.0f;
                c[0] = 250 - 50 * t, c[1] = 235 - 175 * t, c[2] = 230 - 180 * t;
            }
            else
            {
                const float t = (y - 400) / 272.0f;
                c[0] = 60 - 50 * t, c[1] = 45 - 37 * t, c[2] = 40 - 32 * t;
            }
            const float bx = (x - 300) / 140.0f, by = (y - 300) / 140.0f;
            if(bx * bx + by * by < 1)
            {
                const float bz = sqrtf(1 - bx * bx - by * by);
                const float s  = std::max(0.0f, (-bx - by + bz) / 1.732f);
                c[0] = c[1] = c[2] = 25 + 210 * s;
            }
            const float cx = (x - 700) / 180.0f, cy = (y - 470) / 90.0f;
            if(cx * cx + cy * cy < 1)
            {
                const float s = 1 - 0.8f * (cx * cx + cy * cy);
                c[0] = 60 + 180 * s, c[1] = c[2] = 15 + 30 * s;
            }
            if(y >= 610) c[0] = c[1] = c[2] = 80.0f * x / (W - 1);
            const float dx = (x - W / 2.0f) / W, dy = (y - TONE_H / 2.0f) / TONE_H;
            const float vg = 1 - 0.9f * (dx * dx + dy * dy);
            const int   nz = static_cast<int>(mix32(static_cast<uint32_t>(y * W + x)) % 13) - 6;
            int         v[3];
            for(int k = 0; k < 3; k++) v[k] = std::max(0, std::min(255, static_cast<int>(lroundf(c[k] * vg)) + nz));
            put(tc.im, x, y, v[0], v[1], v[2]);
        }
    return tc;
}

// The dark gray ramp with photo pixels at x 320 to 639 and y 50 to 149 and error diffusion on every
// side of them, so the frame has seams in both row directions.
test_case seam()
{
    test_case tc = dark_gray_ramp();
    tc.name      = "s_seam";
    tc.mask.assign(static_cast<size_t>(W) * tc.im.h, 0);
    for(int y = 50; y < 150; y++) memset(&tc.mask[static_cast<size_t>(y) * W + 320], 1, 320);
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

// FreeType text in black at 10 to 64 px, coloured lines, white on black, black on pink, then
// vertical strokes 1 to 4 px wide at gray 0, 64, 128 and 192.
test_case text_block()
{
    test_case tc;
    tc.name                = "d_text";
    tc.im                  = blank(TONE_H);
    const uint8_t black[3] = {0, 0, 0};
    const uint8_t white[3] = {255, 255, 255};
    const uint8_t pink[3]  = {255, 204, 204};
    const char*   pangram  = "The quick brown fox jumps over the lazy dog 0123456789";
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
        fill_rect(tc.im, 0, y, W, y + 28, black);
        ft.draw(tc.im, 20, 8, y + 21, "20 px white on black: The quick brown fox jumps", white);
        y += 30;
        fill_rect(tc.im, 0, y, W, y + 24, pink);
        ft.draw(tc.im, 16, 8, y + 18, "16 px black on #ffcccc: The quick brown fox jumps over the lazy dog", black);
    }
    else
        printf("text face %s did not open, the text image holds only its strokes\n", path);
#else
    (void)black, (void)white, (void)pink, (void)pangram;
    printf("built without FreeType, the text image holds only its strokes\n");
#endif
    const int levels[4] = {0, 64, 128, 192};
    for(int w = 1; w <= 4; w++)
    {
        const int y0 = TONE_H - (5 - w) * 22;
        for(int sec = 0; sec < 4; sec++)
        {
            const int     sx   = sec * (W / 4) + 8;
            const uint8_t v    = static_cast<uint8_t>(levels[sec]);
            const uint8_t c[3] = {v, v, v};
            for(int x = sx; x < (sec + 1) * (W / 4) - 8; x++)
                if((x - sx) % (2 * w) < w)
                    for(int y = y0; y < y0 + 18; y++) blend(tc.im, x, y, c, 255);
        }
    }
    return tc;
}

// ---------------------------------------------------------------- the nearest mix on a made up graph

// Three entries 0, 1 and 2 that all touch, at the corners of a triangle in the plane of L and 2C.
struct toy_graph
{
    int      pl[3] = {600, 3600, 1400}, pc[3] = {100, 300, 1500};
    int      l(unsigned e) const { return pl[e]; }
    int      c(unsigned e) const { return pc[e]; }
    int64_t  d2(unsigned e, int x, int y) const
    {
        return static_cast<int64_t>(pl[e] - x) * (pl[e] - x) + static_cast<int64_t>(pc[e] - y) * (pc[e] - y);
    }
    unsigned owner(int x, int y) const
    {
        unsigned a = 0;
        for(unsigned e = 1; e < 3; e++)
            if(d2(e, x, y) < d2(a, x, y)) a = e;
        return a;
    }
    int toward(unsigned a, int dl, int dc) const
    {
        int    best = -1;
        double cb   = 0;
        for(unsigned e = 0; e < 3; e++)
        {
            const double xl = pl[e] - pl[a], xc = pc[e] - pc[a], dot = xl * dl + xc * dc;
            if(e == a || dot <= 0) continue;
            const double cs = dot / sqrt((xl * xl + xc * xc) * (double(dl) * dl + double(dc) * dc));
            if(best < 0 || cs > cb) best = static_cast<int>(e), cb = cs;
        }
        return best;
    }
    int third(unsigned a, unsigned b, int side) const
    {
        const unsigned t = 3 - a - b;
        const int64_t  o = static_cast<int64_t>(pl[b] - pl[a]) * (pc[t] - pc[a]) -
                          static_cast<int64_t>(pc[b] - pc[a]) * (pl[t] - pl[a]);
        return (o > 0) == (side > 0) ? static_cast<int>(t) : -1;
    }
};

// The distance from (x, y) to the triangle of the toy graph, 0 inside.
double toy_out(const toy_graph& g, double x, double y)
{
    double d = 1e30, s = 0;
    for(int i = 0; i < 3; i++)
    {
        const int    j  = (i + 1) % 3;
        const double ax = g.pl[i], ay = g.pc[i], bx = g.pl[j] - ax, by = g.pc[j] - ay;
        double       t  = ((x - ax) * bx + (y - ay) * by) / (bx * bx + by * by);
        t               = t < 0 ? 0 : t > 1 ? 1 : t;
        d               = std::min(d, std::hypot(x - ax - t * bx, y - ay - t * by));
        s += (bx * (y - ay) - by * (x - ax)) > 0 ? 1 : -1;
    }
    return fabs(s) == 3 ? 0 : d;
}

// 5. The check of the nearest mix on the toy graph. A target on an entry takes that entry at every
//    rank. For targets on a grid around the triangle, the mean of the picks over the 256 ranks
//    lies within the rounding bound of the nearest point of the triangle when it holds the target or
//    takes a third entry, and every pick is an entry of the mix.
bool near_toy()
{
    const toy_graph g;
    unsigned        on_bad = 0, mean_bad = 0, n = 0;
    double          worst  = 0;
    for(unsigned e = 0; e < 3; e++)
    {
        tone_near_mix m;
        tone_near_pick(g, g.l(e), g.c(e), &m);
        for(int r = 0; r < 256; r++) on_bad += tone_near_choose(m, r) != e;
    }
    for(int x = 0; x <= 4096; x += 32)
        for(int y = 0; y <= 1600; y += 16)
        {
            tone_near_mix m;
            tone_near_pick(g, x, y, &m);
            if(m.how != TONE_NEAR_IN && m.how != TONE_NEAR_EDGE_OF && m.how != TONE_NEAR_LINE) continue;
            double ml = 0, mc = 0;
            bool   own = true;
            for(int r = 0; r < 256; r++)
            {
                const unsigned e = tone_near_choose(m, r);
                own              = own && (e == m.v[0] || e == m.v[1] || e == m.v[2]);
                ml += g.l(e) / 256.0, mc += g.c(e) / 256.0;
            }
            // Each cut is placed to 1/512, so the mean misses the point the mix reaches by at most
            // 1/256 of the spread, here below 13, and a mix of two stops within TONE_NEAR_TWO_Q.
            const double reach = m.how == TONE_NEAR_LINE ? TONE_NEAR_TWO_Q : 0;
            const double miss  = std::hypot(ml - x, mc - y) - toy_out(g, x, y) - reach;
            worst              = std::max(worst, miss);
            mean_bad += !own || miss > 14;
            n++;
        }
    const bool good = on_bad == 0 && mean_bad == 0 && n > 0;
    printf("%s  nearest mix on a toy graph: on entry picks off %u, %u of %u targets off the point the mix reaches "
           "(worst %.1f of 14)\n",
           good ? "pass" : "FAIL", on_bad, mean_bad, n, worst);
    return good;
}

} // namespace

int main()
{
    if(!tones_init() || !target_init())
    {
        printf("FAIL  init\n");
        return 1;
    }
    unsigned images = 0, fails = 0;
    auto     check = [&](const test_case& tc, std::vector<uint8_t>* frame) { images++, fails += !run(tc, frame); };
    std::vector<uint8_t> plain;
    check(gray_ramp(), nullptr);
    check(dark_gray_ramp(), &plain);
    check(red_sat_ramp(), nullptr);
    check(red_dark_ramp(), nullptr);
    check(dark_red_ramp(), nullptr);
    check(tri_axes(), nullptr);
    check(tri_show(), nullptr);
    check(text_block(), nullptr);
    check(scene(), nullptr);

    // The photo images in each photo mode that reads the mask, and the seam in all three.
    const char* const    mode_name[TONE_PHOTO_MODES] = {"diffuse", "triangle", "nearest"};
    const test_case      photo[]                     = {as_photo(gray_ramp(), "pa_gray_ramp"),
                                                        as_photo(dark_gray_ramp(), "pa2_dark_gray_ramp"),
                                                        as_photo(red_sat_ramp(), "pb1_red_sat_ramp"),
                                                        as_photo(red_dark_ramp(), "pb2_red_dark_ramp"),
                                                        as_photo(dark_red_ramp(), "pb3_dark_red_ramp"),
                                                        as_photo(tri_show(), "pc2_tri_show"),
                                                        as_photo(scene(), "pe_scene")};
    std::vector<uint8_t> seam_frame[TONE_PHOTO_MODES];
    for(int m = 0; m < TONE_PHOTO_MODES; m++)
    {
        tones_set_photo_mode(static_cast<tone_photo_mode>(m));
        if(m != TONE_PHOTO_DIFFUSE)
            for(test_case tc : photo)
            {
                tc.name += std::string("/") + mode_name[m];
                check(tc, nullptr);
            }
        test_case tc = seam();
        tc.name += std::string("/") + mode_name[m];
        check(tc, &seam_frame[m]);
    }
    tones_set_photo_mode(TONE_PHOTO_DEFAULT);

    // 4. The mode check. The diffuse mode gives the frame of the image without a mask, and the pixels
    // the mask leaves to error diffusion are the same in the two photo modes.
    const std::vector<uint8_t>& tri = seam_frame[TONE_PHOTO_TRIANGLE];
    const std::vector<uint8_t>& nea = seam_frame[TONE_PHOTO_NEAREST];
    const test_case             sm  = seam();
    size_t                      off = 0;
    for(size_t p = 0; p < sm.mask.size(); p++) off += !sm.mask[p] && tri[p] != nea[p];
    const bool modes = seam_frame[TONE_PHOTO_DIFFUSE] == plain && off == 0;
    printf("%s  modes: diffuse seam %s the image without a mask, pixels without the mask that differ between "
           "triangle and nearest %zu\n",
           modes ? "pass" : "FAIL", seam_frame[TONE_PHOTO_DIFFUSE] == plain ? "is" : "is NOT", off);
    images++, fails += !modes;
    images++, fails += !near_toy();
    printf("%s, %u of %u runs failed\n", fails ? "FAIL" : "pass", fails, images);
    return fails ? 1 : 0;
}
