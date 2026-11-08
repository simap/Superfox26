// litehtml container for the 3-color e-paper panel (white, black, red), 960 x 672. Compositing happens
// in a 24-bit RGB band buffer. Each band row is quantized to the palette (quantizer, Atkinson error
// diffusion by default) straight into the epd97 bw and red planes. FreeType for text, libpng and
// libjpeg-turbo for images, fetched over HTTP as the engine asks for them.
// Text: mono glyphs, painted into the band in the text color and dithered with everything else.

#include "render.h"

#include <litehtml.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epd97.h"
#include "fetch.h"
#include "fonts.h"
#include "image.h"
#include "leds.h"
#include "sizecache.h"
#include "tabs.h"
#include "url.h"

using namespace litehtml;

static const char* TAG = "render";

static const int VIEW_W    = EPD_W; // 960
// The panel splits into the tab bar (rows 0 to TAB_H - 1) and the page window below it. PAGE_H is the
// page document's viewport height for vh units and media queries.
static const int PAGE_H    = EPD_H - TAB_H;
static const int BAND_ROWS = 64;    // RGB band 184,320 bytes

static const size_t IMG_KEEP     = 192 * 1024; // decoded image bytes kept across bands
static const size_t IMG_FILE_MAX = 300 * 1024; // compressed bytes fetched per image
static const size_t IMG_SRC_MAX  = 1024 * 1024; // compressed bytes held per page, all in-view images
// Header probe for the natural size. All PNG, GIF, BMP and WebP headers and 32 of 33 measured JPEGs
// fit in 3.3 KB; one JPEG with EXIF and Photoshop blocks needed 15 KB. A JPEG whose frame header is
// still missing after the first probe gets one more at PROBE_BYTES_2.
static const size_t PROBE_BYTES   = 16 * 1024;
static const size_t PROBE_BYTES_2 = 64 * 1024;
static const int    RETRY_DELAY_MS = 500; // before the second attempt at an image fetch
static const size_t CSS_MAX      = 400 * 1024; // one stylesheet
static const size_t GLYPH_CACHE  = 256 * 1024;
// Free PSRAM that must remain after a phase's declared bytes. The engine reports the stylesheet
// size at the cascade check, but cascade, render tree and layout grew the heap by 1.3 MB beyond
// that on Wikipedia (3.4 MB after parse, 4.7 MB peak), and the layout and draw phases that follow
// have no check of their own. With exceptions off an allocation failure aborts the firmware.
static const size_t BUDGET_MARGIN = 2560 * 1024;

// User stylesheet, applied after the page's own styles.
// Wikipedia's Vector 2022 skin gives these two elements height:100% inside auto-height flex
// containers. litehtml resolves the percentage against the 672 px viewport (document.cpp passes
// the viewport height down as the containing block height), so each one opens a blank band of
// about 700 px above the article. With height:auto the heading lands at y 135.
static const char* USER_CSS = ".mw-logo, #p-lang-btn-checkbox { height: auto !important; }";

// ---------------------------------------------------------------- utilities

struct rgb
{
    uint8_t r = 255, g = 255, b = 255;
};

static rgb to_rgb(const web_color& c) { return {c.red, c.green, c.blue}; }

struct rect_i
{
    int x0, y0, x1, y1; // half open
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

static rect_i to_rect(const position& p)
{
    return {static_cast<int>(std::floor(p.x.value())), static_cast<int>(std::floor(p.y.value())),
            static_cast<int>(std::floor(p.x.value() + p.width.value() + 0.5f)),
            static_cast<int>(std::floor(p.y.value() + p.height.value() + 0.5f))};
}

static rect_i intersect(const rect_i& a, const rect_i& b)
{
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

static uint32_t utf8_next(const char*& s)
{
    auto     p = reinterpret_cast<const unsigned char*>(s);
    uint32_t c = *p++;
    int      extra = 0;
    if(c >= 0xF0) { c &= 0x07; extra = 3; }
    else if(c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if(c >= 0xC0) { c &= 0x1F; extra = 1; }
    while(extra-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    s = reinterpret_cast<const char*>(p);
    return c;
}

// default-ignorable format characters: zero width, never drawn
static bool ignorable(uint32_t c)
{
    return c == 0xAD || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF;
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_decode(const std::string& in)
{
    std::string out;
    int         val = 0, bits = -8;
    for(unsigned char c : in)
    {
        const char* p = strchr(b64tab, c);
        if(!p || !c) continue;
        val = (val << 6) + static_cast<int>(p - b64tab);
        bits += 6;
        if(bits >= 0)
        {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// ---------------------------------------------------------------- palette and quantizer

enum { PAL_WHITE = 0, PAL_BLACK = 1, PAL_RED = 2 };

// Ordered dither of one RGB pixel to the palette {white, black, red}.
//   t  = bayer8[y & 7][x & 7], 0..63
//   b  = 255 * (2t - 63) / 128, a signed bias in [-125.5, +125.5], the same for all three channels
//   c' = c + b per channel (not clamped)
//   nearest palette entry P by d(P) = wr (r' - Pr)^2 + wg (g' - Pg)^2 + wb (b' - Pb)^2,
//   wr = 0.299, wg = 0.587, wb = 0.114
// Each palette channel is 0 or 255, so the pairwise tests reduce to:
//   red   nearer than black  iff  r' > 127.5
//   red   nearer than white  iff  (wg g' + wb b') / (wg + wb) < 127.5
//   white nearer than black  iff  wr r' + wg g' + wb b' > 127.5
// Result: red if both red tests hold (strictly). Otherwise white if the third test holds, else black.
// Grays never pick red. Palette colors map to themselves at every threshold (|b| < 127.5).
// Integer form below: every value is scaled by 128, so 127.5 becomes 16320.
static const int Q_HALF = 16320;

static inline int pal_index128(int r2, int g2, int b2)
{
    if(r2 > Q_HALF && 587 * g2 + 114 * b2 < 701 * Q_HALF) return PAL_RED;
    return 299 * r2 + 587 * g2 + 114 * b2 > 1000 * Q_HALF ? PAL_WHITE : PAL_BLACK;
}

static inline int quantize_px(int r, int g, int b, int x, int y)
{
    int bias = 255 * (2 * bayer8[y & 7][x & 7] - 63);
    return pal_index128(r * 128 + bias, g * 128 + bias, b * 128 + bias);
}

// ---------------------------------------------------------------- quantizer
// Port of the quantizer on branch dither of the device container (harness/device/bwr_main.cpp).
// DITHER_BAYER: quantize_px above. DITHER_FS: Floyd-Steinberg. DITHER_ATKINSON: Atkinson.
// Error diffusion scans serpentine (odd rows right to left), picks the nearest palette entry (same weighted
// test, no bias) for c + e, and diffuses the RGB error c + e - P. FS: 7/16 ahead, 3/16 below behind, 5/16
// below, 1/16 below ahead. Atkinson: 1/8 each to 1 and 2 ahead, below behind, below, below ahead, 2 below
// (6/8 diffused). The incoming error e is clamped per channel to +-ERR_CLAMP and so that c + e stays in
// 0..255. Every palette entry has g = b, so the g - b part of an error can never be paid back and would
// otherwise grow without bound and smear across edges. Without the 0..255 bound, an error below black in r
// and b is passed on unchanged by every black pixel and never decays: a saturated green area locks to solid
// black. The error rows persist from row to row, so they carry across band boundaries. alloc() resets them
// for each window.
// SNAP: a pixel within SNAP of a palette entry (max per-channel abs difference, source color before any
// error) is written as that entry: no bias, no incoming error used, no error diffused. 0 disables it.
enum { DITHER_BAYER = 0, DITHER_FS = 1, DITHER_ATKINSON = 2 };
static const int DITHER_MODE = DITHER_ATKINSON;
static const int SNAP        = 0;
static const int ERR_CLAMP   = 128;

static const uint8_t PALETTE[3][3] = {{255, 255, 255}, {0, 0, 0}, {255, 0, 0}};

struct quantizer
{
    int w = 0;
    // error rows, (w + 4) x 3 int16 each, in 1/16 pixel units, index x + 2
    std::vector<int16_t> rows[3];

    void alloc(int w_)
    {
        w      = w_;
        int nr = DITHER_MODE == DITHER_FS ? 2 : DITHER_MODE == DITHER_ATKINSON ? 3 : 0;
        for(int i = 0; i < 3; i++)
        {
            if(i < nr) rows[i].assign(static_cast<size_t>(w + 4) * 3, 0);
            else std::vector<int16_t>().swap(rows[i]);
        }
    }
    void release()
    {
        for(auto& r : rows) std::vector<int16_t>().swap(r);
    }

    // palette index if c is within SNAP of an entry, else -1
    static int snapped(int r, int g, int b)
    {
        if(SNAP <= 0) return -1;
        for(int i = 0; i < 3; i++)
            if(std::abs(r - PALETTE[i][0]) <= SNAP && std::abs(g - PALETTE[i][1]) <= SNAP &&
               std::abs(b - PALETTE[i][2]) <= SNAP)
                return i;
        return -1;
    }

    // One RGB row into one row of each plane: bit 7 of byte 0 is x = 0, a set bw bit is ink, a set red bit
    // is red. Both rows are cleared first.
    void row(const uint8_t* c, int y, uint8_t* bw, uint8_t* red)
    {
        memset(bw, 0, static_cast<size_t>(w + 7) / 8);
        memset(red, 0, static_cast<size_t>(w + 7) / 8);
        auto put = [&](int x, int idx) {
            if(idx == PAL_BLACK) bw[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
            else if(idx == PAL_RED) red[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
        };
        if(DITHER_MODE == DITHER_BAYER)
        {
            for(int x = 0; x < w; x++)
            {
                int r = c[3 * x], g = c[3 * x + 1], b = c[3 * x + 2];
                int s = snapped(r, g, b);
                put(x, s >= 0 ? s : quantize_px(r, g, b, x, y));
            }
            return;
        }
        bool     atk = DITHER_MODE == DITHER_ATKINSON;
        int16_t* cur = rows[0].data();
        int16_t* nxt = rows[1].data();
        int16_t* nx2 = atk ? rows[2].data() : nullptr;
        int      d   = (y & 1) ? -1 : 1;
        for(int i = 0; i < w; i++)
        {
            int x = d > 0 ? i : w - 1 - i;
            int s = snapped(c[3 * x], c[3 * x + 1], c[3 * x + 2]);
            if(s >= 0)
            {
                put(x, s);
                continue;
            }
            int v[3], e[3];
            for(int k = 0; k < 3; k++)
            {
                int ck = c[3 * x + k];
                int in = cur[(x + 2) * 3 + k] / 16;
                in     = std::max(-std::min(ERR_CLAMP, ck), std::min(std::min(ERR_CLAMP, 255 - ck), in));
                v[k]   = ck + in;
            }
            int idx = pal_index128(v[0] * 128, v[1] * 128, v[2] * 128);
            put(x, idx);
            e[0] = v[0] - PALETTE[idx][0];
            e[1] = v[1] - PALETTE[idx][1];
            e[2] = v[2] - PALETTE[idx][2];
            auto add = [&](int16_t* r, int xx, int k, int amt) {
                if(xx >= 0 && xx < w) r[(xx + 2) * 3 + k] = static_cast<int16_t>(r[(xx + 2) * 3 + k] + amt);
            };
            for(int k = 0; k < 3; k++)
            {
                if(!atk)
                {
                    add(cur, x + d, k, e[k] * 7);
                    add(nxt, x - d, k, e[k] * 3);
                    add(nxt, x, k, e[k] * 5);
                    add(nxt, x + d, k, e[k]);
                } else
                {
                    int a = e[k] * 2; // 1/8 in 1/16 units
                    add(cur, x + d, k, a);
                    add(cur, x + 2 * d, k, a);
                    add(nxt, x - d, k, a);
                    add(nxt, x, k, a);
                    add(nxt, x + d, k, a);
                    add(nx2, x, k, a);
                }
            }
        }
        // shift the error rows up one row
        std::swap(rows[0], rows[1]);
        if(atk) std::swap(rows[1], rows[2]);
        std::fill(atk ? rows[2].begin() : rows[1].begin(), atk ? rows[2].end() : rows[1].end(), 0);
    }
};

// ---------------------------------------------------------------- fonts

struct font
{
    int          slot = FACE_SANS;
    int          px = 16;
    int          decoration = 0;
    font_metrics fm;
};

// ---------------------------------------------------------------- container

class bwr_container : public document_container
{
  public:
    int                  W = VIEW_W, H = 0;
    int                  view_h; // viewport height reported to the engine
    std::string          caption; // <title> text, from set_caption
    struct icon_cand
    {
        std::string href;
        int         score; // lower is better
    };
    std::vector<icon_cand> icons; // <link rel=icon> candidates, from link()
    std::vector<uint8_t> fb; // RGB band, 3 bytes per pixel
    std::vector<rect_i>  clips;
    std::string          page_url; // the document URL, for relative references
    std::string          base_url; // <base href>, else page_url
    face_set&            fonts;
    glyph_cache          gcache{GLYPH_CACHE};
    render_stats*        st = nullptr;
    long                 n_measure = 0, n_draw_text = 0, n_fonts = 0;

    // An image source. The natural size comes from the size cache or from a header probe at load_image
    // time, only when layout needs it (an <img> without CSS or attribute dimensions); the rest wait.
    // The file is fetched only for images the window draws: a collect pass over the window marks them
    // WANTED, render_page fetches those, and the data is freed after the last band that uses it.
    // Pixels are decoded on first draw at the drawn size.
    enum img_state { IMG_DEFERRED, IMG_SIZED, IMG_WANTED, IMG_LOADED, IMG_FAILED };
    struct img_src
    {
        int         kind = IMG_NONE;
        int         nat_w = 0, nat_h = 0;
        img_state   state    = IMG_DEFERRED; // DEFERRED only until load_image has probed or looked it up
        bool        need_nat = false;        // layout used the natural size: a mismatch at fetch means relayout
        int64_t     total    = -1;    // file size from the probe response, -1 unknown
        int         last_row = 0;     // last window row (exclusive) any draw needs, from the collect pass
        std::string data;             // the whole file while LOADED (or a data: URL's bytes)
        std::vector<std::unique_ptr<gimage>> vars; // decoded copies, one per display size in use
    };
    std::map<std::string, std::unique_ptr<img_src>> images;
    size_t img_bytes = 0, img_count = 0, img_fail = 0, img_src_bytes = 0;
    long   img_tick = 0;
    // 0 parse, 1 layout, 2 collect pass, 3 draw. get_image_size fetches a deferred file only from 2 on.
    int  phase   = 0;
    bool collect = false; // the draw calls record wanted images and paint nothing
    bool size_mismatch = false; // a fetched file's size differed from the probed or cached one

    bwr_container(face_set& f, const std::string& url, int vh) : view_h(vh), page_url(url), base_url(url), fonts(f) {}
    ~bwr_container() override { gcache.clear(); }

    // fb is an RGB band buffer of W x band rows; window rows band_y0..band_y1 (half open) map onto it.
    int       band_y0 = 0, band_y1 = 0;
    quantizer q;

    bool alloc_window(int band_rows)
    {
        fb.assign(static_cast<size_t>(W) * band_rows * 3, 255);
        q.alloc(W);
        return fb.size() == static_cast<size_t>(W) * band_rows * 3;
    }
    void free_window()
    {
        std::vector<uint8_t>().swap(fb);
        q.release();
    }

    void begin_band(int y0, int y1, int page_h)
    {
        H       = page_h;
        band_y0 = y0;
        band_y1 = y1;
        if(!collect) std::fill(fb.begin(), fb.end(), 255);
        clips.clear();
    }

    rect_i cur_clip() const
    {
        rect_i r{0, band_y0, W, band_y1};
        if(!clips.empty()) r = intersect(r, clips.back());
        return r;
    }

    void blend(int x, int y, const rgb& c, int a)
    {
        if(a <= 0 || collect) return;
        uint8_t* d = &fb[(static_cast<size_t>(y - band_y0) * W + x) * 3];
        if(a >= 255)
        {
            d[0] = c.r;
            d[1] = c.g;
            d[2] = c.b;
            return;
        }
        d[0] = static_cast<uint8_t>((c.r * a + d[0] * (255 - a)) / 255);
        d[1] = static_cast<uint8_t>((c.g * a + d[1] * (255 - a)) / 255);
        d[2] = static_cast<uint8_t>((c.b * a + d[2] * (255 - a)) / 255);
    }

    void fill(const rect_i& r0, const rgb& v, int a)
    {
        rect_i r = intersect(r0, cur_clip());
        for(int y = r.y0; y < r.y1; y++)
            for(int x = r.x0; x < r.x1; x++) blend(x, y, v, a);
    }

    static bool rounded(const border_radiuses& r)
    {
        return r.top_left_x > 0_px || r.top_right_x > 0_px || r.bottom_left_x > 0_px || r.bottom_right_x > 0_px;
    }

    // Horizontal extent [x0, x1) of the rounded rectangle box on row y: each corner is a quarter
    // ellipse with the box's radii. False when the row is outside the box.
    static bool rounded_span(const rect_i& box, const border_radiuses& r, int y, int& x0, int& x1)
    {
        if(y < box.y0 || y >= box.y1) return false;
        x0 = box.x0;
        x1 = box.x1;
        auto inset = [](float rx, float ry, float d) {
            // d: distance of the row centre from the corner's straight edge, 0 .. ry
            if(rx <= 0 || ry <= 0) return 0.0f;
            float t = (ry - d) / ry;
            return rx * (1.0f - std::sqrt(std::max(0.0f, 1.0f - t * t)));
        };
        float yc = y + 0.5f;
        float tl = r.top_left_y.value(), tr = r.top_right_y.value();
        float bl = r.bottom_left_y.value(), br = r.bottom_right_y.value();
        if(yc < box.y0 + tl) x0 = box.x0 + static_cast<int>(std::lround(inset(r.top_left_x.value(), tl, yc - box.y0)));
        if(yc > box.y1 - bl) x0 = std::max(x0, box.x0 + static_cast<int>(std::lround(inset(r.bottom_left_x.value(), bl, box.y1 - yc))));
        if(yc < box.y0 + tr) x1 = box.x1 - static_cast<int>(std::lround(inset(r.top_right_x.value(), tr, yc - box.y0)));
        if(yc > box.y1 - br) x1 = std::min(x1, box.x1 - static_cast<int>(std::lround(inset(r.bottom_right_x.value(), br, box.y1 - yc))));
        return x1 > x0;
    }

    void fill_rounded(const rect_i& box, const border_radiuses& r, const rgb& v, int a)
    {
        rect_i clip = intersect(box, cur_clip());
        for(int y = clip.y0; y < clip.y1; y++)
        {
            int x0, x1;
            if(!rounded_span(box, r, y, x0, x1)) continue;
            for(int x = std::max(x0, clip.x0); x < std::min(x1, clip.x1); x++) blend(x, y, v, a);
        }
    }

    void note_psram()
    {
        size_t f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if(st && (st->psram_low == 0 || f < st->psram_low)) st->psram_low = f;
    }

    // ---- URL resolution: against the stylesheet URL the engine passes, else the document base
    std::string resolve(const char* src, const char* baseurl)
    {
        if(!src || !*src) return "";
        return resolve_url(baseurl && *baseurl ? std::string(baseurl) : base_url, src);
    }

    img_src* find_image(const char* src, const char* baseurl)
    {
        auto it = images.find(resolve(src, baseurl));
        return it == images.end() || !it->second || !it->second->kind ? nullptr : it->second.get();
    }

    // ---- memory budget, called by createFromString before parse, cascade and tree
    bool check_memory_budget(const char* stage, size_t bytes) override
    {
        size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        bool   ok      = free_ps > bytes + BUDGET_MARGIN;
        ESP_LOGI(TAG, "budget %s: needs %u + %u margin, free PSRAM %u, largest block %u, images %u: %s", stage,
                 (unsigned)bytes, (unsigned)BUDGET_MARGIN, (unsigned)free_ps,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)img_src_bytes,
                 ok ? "accept" : "refuse");
        note_psram();
        return ok;
    }

    // ---- fonts
    uint_ptr create_font(const font_description& d, const document*, font_metrics* fm) override
    {
        n_fonts++;
        std::string fam = d.family;
        std::transform(fam.begin(), fam.end(), fam.begin(), ::tolower);
        // pick the first generic class that matches anywhere in the family list
        std::string cls = "sans";
        size_t      best = std::string::npos;
        auto        probe = [&](const char* key, const char* c) {
            auto p = fam.find(key);
            if(p != std::string::npos && p < best)
            {
                best = p;
                cls  = c;
            }
        };
        probe("mono", "mono");
        probe("courier", "mono");
        probe("menlo", "mono");
        probe("consolas", "mono");
        probe("times", "serif");
        probe("georgia", "serif");
        probe("sans", "sans");
        probe("arial", "sans");
        probe("helvetica", "sans");
        probe("verdana", "sans");
        {
            // "serif" alone (not part of "sans-serif")
            size_t p = 0;
            while((p = fam.find("serif", p)) != std::string::npos)
            {
                if((p < 5 || fam.compare(p - 5, 5, "sans-") != 0) && p < best)
                {
                    best = p;
                    cls  = "serif";
                }
                p += 5;
            }
        }
        bool bold = d.weight >= 600;

        auto f        = new font();
        f->slot       = cls == "mono" ? (bold ? FACE_MONO_BOLD : FACE_MONO) : (bold ? FACE_BOLD : FACE_SANS);
        f->px         = std::max(1, static_cast<int>(std::lround(d.size.value())));
        f->decoration = d.decoration_line;
        FT_Face face  = fonts.sized(f->slot, f->px);
        if(!face)
        {
            delete f;
            return 0;
        }
        FT_Size_Metrics& m = face->size->metrics;
        f->fm.font_size    = d.size;
        f->fm.ascent       = static_cast<float>((m.ascender + 63) >> 6);
        f->fm.descent      = static_cast<float>((-m.descender + 63) >> 6);
        f->fm.height       = f->fm.ascent + f->fm.descent;
        f->fm.x_height     = static_cast<float>(f->px) * 0.5f;
        if(!FT_Load_Char(face, 'x', FT_LOAD_DEFAULT))
            f->fm.x_height = static_cast<float>(face->glyph->metrics.height) / 64.0f;
        f->fm.ch_width = static_cast<float>(f->px) * 0.5f;
        if(!FT_Load_Char(face, '0', FT_LOAD_DEFAULT))
            f->fm.ch_width = static_cast<float>(face->glyph->advance.x) / 64.0f;
        f->fm.draw_spaces = (f->decoration != 0);
        f->fm.sub_shift   = static_cast<float>(f->px) / 5.0f;
        f->fm.super_shift = static_cast<float>(f->px) / 3.0f;
        if(fm) *fm = f->fm;
        return reinterpret_cast<uint_ptr>(f);
    }

    void delete_font(uint_ptr h) override
    {
        auto f = reinterpret_cast<font*>(h);
        delete f;
    }

    // Cached glyph for code point c in font f. With need_bitmap the entry carries the coverage bitmap.
    // No fallback face: a code point the face lacks gets the face's .notdef glyph.
    glyph_cache::entry* glyph(font* f, uint32_t c, bool need_bitmap)
    {
        uint64_t key = (static_cast<uint64_t>(f->slot) << 40) | (static_cast<uint64_t>(f->px & 0xFFFF) << 21) |
                       (c & 0x1FFFFF);
        glyph_cache::entry* e = gcache.find(key);
        if(e && (!need_bitmap || e->rendered)) return e;
        FT_Face face = fonts.sized(f->slot, f->px);
        if(!face) return nullptr;
        FT_UInt gi = FT_Get_Char_Index(face, c);
        // The mono target hints differently and can change the advance: the advance always comes from the
        // default-target load that text_width uses, so layout and drawing agree.
        if(!e)
        {
            if(FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT)) return nullptr;
            e      = gcache.insert(key);
            e->adv = static_cast<int32_t>(face->glyph->advance.x);
        }
        if(need_bitmap)
        {
            if(FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)) return nullptr;
            FT_GlyphSlot     g  = face->glyph;
            const FT_Bitmap& bm = g->bitmap;
            e->left             = static_cast<int16_t>(g->bitmap_left);
            e->top              = static_cast<int16_t>(g->bitmap_top);
            e->w                = static_cast<uint16_t>(bm.width);
            e->h                = static_cast<uint16_t>(bm.rows);
            e->rendered         = true;
            size_t n            = static_cast<size_t>(e->w) * e->h;
            if(n)
            {
                // stored as one byte per pixel, 0 or 255
                e->bm.reset(new uint8_t[n]);
                for(unsigned y = 0; y < bm.rows; y++)
                {
                    const uint8_t* s = bm.buffer + static_cast<long>(y) * bm.pitch;
                    uint8_t*       d = &e->bm[static_cast<size_t>(y) * e->w];
                    for(unsigned x = 0; x < bm.width; x++)
                    {
                        if(bm.pixel_mode == FT_PIXEL_MODE_MONO) d[x] = ((s[x >> 3] >> (7 - (x & 7))) & 1) ? 255 : 0;
                        else d[x] = s[x] >= 128 ? 255 : 0;
                    }
                }
            }
            gcache.grow(n);
        }
        return e;
    }

    pixel_t text_width(const char* text, uint_ptr h) override
    {
        n_measure++;
        auto   f = reinterpret_cast<font*>(h);
        FT_Pos w = 0;
        for(const char* p = text; *p;)
        {
            uint32_t c = utf8_next(p);
            if(ignorable(c)) continue;
            if(auto e = glyph(f, c, false)) w += e->adv;
        }
        return static_cast<float>(w) / 64.0f;
    }

    void draw_text(uint_ptr, const char* text, uint_ptr h, web_color color, const position& pos) override
    {
        if(collect) return;
        n_draw_text++;
        // the text color goes into the band at full depth like a fill; mono glyph coverage is 0 or 255,
        // so the quantizer dithers text pixels with everything else
        auto   f    = reinterpret_cast<font*>(h);
        rgb    v    = to_rgb(color);
        int    a    = color.alpha;
        rect_i clip = cur_clip();
        int    base = static_cast<int>(std::lround(pos.y.value() + pos.height.value() - f->fm.descent.value()));
        FT_Pos pen  = static_cast<FT_Pos>(std::lround(pos.x.value() * 64.0f));
        for(const char* p = text; *p;)
        {
            uint32_t c = utf8_next(p);
            if(ignorable(c)) continue;
            auto e = glyph(f, c, true);
            if(!e) continue;
            int gx = static_cast<int>(pen >> 6) + e->left;
            int gy = base - e->top;
            for(int yy = 0; yy < e->h; yy++)
            {
                int y = gy + yy;
                if(y < clip.y0 || y >= clip.y1) continue;
                for(int xx = 0; xx < e->w; xx++)
                {
                    int x = gx + xx;
                    if(x < clip.x0 || x >= clip.x1) continue;
                    int cov = e->bm[static_cast<size_t>(yy) * e->w + xx];
                    blend(x, y, v, cov * a / 255);
                }
            }
            pen += e->adv;
        }
        int th = std::max(1, f->px / 14);
        int x0 = static_cast<int>(std::lround(pos.x.value()));
        int x1 = static_cast<int>(pen >> 6);
        if(f->decoration & text_decoration_line_underline) fill({x0, base + 1, x1, base + 1 + th}, v, a);
        if(f->decoration & text_decoration_line_line_through)
        {
            int y = base - static_cast<int>(f->fm.x_height.value() / 2);
            fill({x0, y, x1, y + th}, v, a);
        }
        if(f->decoration & text_decoration_line_overline)
        {
            int y = base - static_cast<int>(f->fm.ascent.value());
            fill({x0, y, x1, y + th}, v, a);
        }
    }

    pixel_t     pt_to_px(float pt) const override { return pt * 96.0f / 72.0f; }
    pixel_t     get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "Times New Roman"; }

    void draw_list_marker(uint_ptr, const list_marker& m) override
    {
        if(!m.image.empty())
        {
            if(img_src* s = find_image(m.image.c_str(), m.baseurl))
            {
                // the engine calls this for every item whose parent meets the clip, so the marker itself
                // is tested here before anything is fetched or decoded
                rect_i r = to_rect(m.pos);
                if(intersect(r, cur_clip()).empty()) return;
                if(collect)
                {
                    want(*s, r.y1);
                    return;
                }
                if(gimage* img = variant(*s, r.x1 - r.x0, r.y1 - r.y0, r.y1)) blit(*img, r, cur_clip());
                return;
            }
        }
        if(collect) return;
        rect_i r = to_rect(m.pos);
        rgb    v = to_rgb(m.color);
        switch(m.marker_type)
        {
        case list_style_type_square:
            fill(r, v, m.color.alpha);
            break;
        case list_style_type_circle:
        case list_style_type_disc:
        {
            float cx = (r.x0 + r.x1) / 2.0f, cy = (r.y0 + r.y1) / 2.0f, rad = (r.x1 - r.x0) / 2.0f;
            rect_i cl = cur_clip();
            for(int y = r.y0; y < r.y1; y++)
                for(int x = r.x0; x < r.x1; x++)
                {
                    if(x < cl.x0 || x >= cl.x1 || y < cl.y0 || y >= cl.y1) continue;
                    float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                    bool  on = m.marker_type == list_style_type_disc ? d <= rad : (d <= rad && d >= rad - 1.2f);
                    if(on) blend(x, y, v, m.color.alpha);
                }
            break;
        }
        default:
            break;
        }
    }

    // ---- images
    // Called during parse for every image the document references. Every image gets its natural size
    // here, from the cache or from a header probe: layout uses it even when CSS or attributes give both
    // dimensions (render_item_image::calc_intrinsic_size feeds table and flex sizing from it, and a 0x0
    // answer collapsed the weather.gov forecast icons). No image file is fetched here.
    void load_image(const char* src, const char* baseurl, bool) override
    {
        std::string url = resolve(src, baseurl);
        if(url.empty() || images.count(url)) return;
        auto s      = std::make_unique<img_src>();
        s->need_nat = true;
        img_count++;
        if(url.compare(0, 5, "data:") == 0)
        {
            auto comma = url.find(',');
            if(comma != std::string::npos && url.rfind(";base64", comma) != std::string::npos)
                s->data = b64_decode(url.substr(comma + 1));
            s->kind = s->data.size() > 8 ? sniff_size(s->data, s->nat_w, s->nat_h) : IMG_NONE;
            if(s->kind && s->nat_w > 0 && s->nat_h > 0)
            {
                shrink_to_fit(s->data);
                s->state = IMG_LOADED;
                img_src_bytes += s->data.size();
            } else
                fail(*s);
        } else
        {
            // .svg never decodes; skip the fetch
            auto q    = url.find('?');
            auto path = q == std::string::npos ? url : url.substr(0, q);
            bool svg  = path.size() > 4 && path.compare(path.size() - 4, 4, ".svg") == 0;
            if(svg) fail(*s);
            else if(sizecache_get(url, s->kind, s->nat_w, s->nat_h))
            {
                s->state = IMG_SIZED;
                if(st) st->img_cache_hits++;
            } else probe(url, *s);
        }
        note_psram();
        images[url] = std::move(s);
    }

    void fail(img_src& s)
    {
        s.state = IMG_FAILED;
        s.kind  = IMG_NONE;
        s.nat_w = s.nat_h = 0;
        std::string().swap(s.data);
        img_fail++;
    }

    // An image fetch that fails is tried once more after a pause, unless the server said the resource
    // does not exist. Covers a dropped keep-alive connection, a timeout and a 5xx.
    static bool definite(const fetch_result& r) { return r.status == 404 || r.status == 410 || r.status == 403; }

    static bool fetch_prefix_retry(const std::string& url, std::string& out, size_t n, fetch_result& r)
    {
        if(fetch_prefix(url, out, n, &r) || definite(r)) return r.status >= 200 && r.status < 300;
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        return fetch_prefix(url, out, n, &r);
    }

    static bool fetch_url_retry(const std::string& url, std::string& out, size_t max, fetch_result& r)
    {
        if(fetch_url(url, out, max, &r) || definite(r) || r.truncated) return !r.truncated && r.status >= 200 && r.status < 300;
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        return fetch_url(url, out, max, &r);
    }

    // Header probe: the first PROBE_BYTES, once more at PROBE_BYTES_2 for a JPEG whose frame header
    // came later. Sets kind and size, records the size in the cache, keeps no bytes. A failed probe is
    // not cached, so the next page load tries again.
    void probe(const std::string& url, img_src& s)
    {
        if(st) st->img_probes++;
        std::string  head;
        fetch_result r;
        bool         incomplete = false;
        if(fetch_prefix_retry(url, head, PROBE_BYTES, r))
        {
            s.total = r.total;
            s.kind  = sniff_size(head, s.nat_w, s.nat_h, &incomplete);
            if(!s.kind && incomplete && r.partial && fetch_prefix_retry(url, head, PROBE_BYTES_2, r))
                s.kind = sniff_size(head, s.nat_w, s.nat_h, &incomplete);
        }
        if(s.kind && s.nat_w > 0 && s.nat_h > 0)
        {
            s.state = IMG_SIZED;
            sizecache_put(url, s.kind, s.nat_w, s.nat_h);
        } else
        {
            ESP_LOGW(TAG, "no size for %s: HTTP %d, %u bytes%s", url.c_str(), r.status, (unsigned)head.size(),
                     incomplete ? ", header past the probe" : "");
            fail(s);
        }
    }

    // Collect pass: the window needs this image down to window row bottom.
    void want(img_src& s, int bottom)
    {
        if(s.state == IMG_FAILED) return;
        if(s.state != IMG_LOADED) s.state = IMG_WANTED;
        s.last_row = std::max(s.last_row, bottom);
    }

    // Fetch the whole file of a wanted or deferred image, sniff it, and keep it for drawing. A size that
    // differs from the probe or cache is recorded and, when layout used it, flagged for a relayout.
    bool load_full(const std::string& url, img_src& s)
    {
        if(s.state == IMG_LOADED) return true;
        if(s.state == IMG_FAILED) return false;
        if(s.total > static_cast<int64_t>(IMG_FILE_MAX) || img_src_bytes >= IMG_SRC_MAX)
        {
            ESP_LOGW(TAG, "skip %s: %lld bytes, %u held", url.c_str(), (long long)s.total, (unsigned)img_src_bytes);
            if(st) st->img_skipped++;
            fail(s);
            return false;
        }
        fetch_result r;
        bool         ok = fetch_url_retry(url, s.data, std::min(IMG_FILE_MAX, IMG_SRC_MAX - img_src_bytes), r);
        int          w = 0, h = 0;
        int          kind = s.data.size() > 8 ? sniff_size(s.data, w, h) : IMG_NONE;
        // a truncated GIF still shows its decoded rows; other formats need the whole file
        if(!ok && !(r.truncated && kind == IMG_GIF)) kind = IMG_NONE;
        if(!kind || w <= 0 || h <= 0)
        {
            ESP_LOGW(TAG, "image %s: HTTP %d, %u bytes%s, not decodable", url.c_str(), r.status,
                     (unsigned)s.data.size(), r.truncated ? " (truncated)" : "");
            if(r.truncated && st) st->img_skipped++;
            fail(s);
            return false;
        }
        if(s.state == IMG_SIZED || s.state == IMG_WANTED)
        {
            if(s.nat_w && (w != s.nat_w || h != s.nat_h))
            {
                ESP_LOGW(TAG, "image %s is %dx%d, size was %dx%d", url.c_str(), w, h, s.nat_w, s.nat_h);
                if(s.need_nat) size_mismatch = true;
            }
        }
        s.kind  = kind;
        s.nat_w = w;
        s.nat_h = h;
        sizecache_put(url, kind, w, h);
        shrink_to_fit(s.data);
        s.state = IMG_LOADED;
        img_src_bytes += s.data.size();
        if(st) st->img_fetched_bytes += s.data.size();
        note_psram();
        return true;
    }

    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override
    {
        sz.width  = 0;
        sz.height = 0;
        std::string url = resolve(src, baseurl);
        auto        it  = images.find(url);
        if(it == images.end()) return;
        img_src& s = *it->second;
        // Backgrounds and list images ask at draw time, from boxes that meet the clip: fetch then.
        if(s.state == IMG_DEFERRED && phase >= 2) load_full(url, s);
        if(s.state == IMG_FAILED) return;
        sz.width  = s.nat_w;
        sz.height = s.nat_h;
    }

    // decoded copy of s for display size tw x th; decodes on first use. bottom: last page row this draw needs.
    gimage* variant(img_src& s, int tw, int th, int bottom)
    {
        if(tw <= 0 || th <= 0 || s.state != IMG_LOADED) return nullptr;
        img_tick++;
        for(auto& v : s.vars)
            if(v->req_w == tw && v->req_h == th)
            {
                v->last_y   = std::max(v->last_y, bottom);
                v->last_use = img_tick;
                return v->px.empty() ? nullptr : v.get();
            }
        auto v      = std::make_unique<gimage>();
        v->req_w    = tw;
        v->req_h    = th;
        v->last_y   = bottom;
        v->last_use = img_tick;
        int dw = std::min(tw, s.nat_w), dh = std::min(th, s.nat_h);
        if(!decode_image(s.kind, s.data, *v, dw, dh))
            ESP_LOGW(TAG, "decode failed, kind %d, %dx%d for %dx%d", s.kind, s.nat_w, s.nat_h, tw, th);
        note_psram();
        img_bytes += v->bytes();
        s.vars.push_back(std::move(v));
        return s.vars.back()->px.empty() ? nullptr : s.vars.back().get();
    }

    // After a band: the files of images whose last window row is in this band or above are freed, and
    // decoded copies no later band has asked for yet (last_y <= y1) are freed, least recently used first,
    // until the bytes held fit in IMG_KEEP.
    void end_band(int y1)
    {
        for(auto& kv : images)
        {
            img_src& s = *kv.second;
            if(s.state == IMG_LOADED && s.last_row <= y1 && !s.data.empty() && kv.first.compare(0, 5, "data:") != 0)
            {
                img_src_bytes -= s.data.size();
                std::string().swap(s.data);
            }
        }
        for(;;)
        {
            if(img_bytes <= IMG_KEEP) break;
            std::vector<std::unique_ptr<gimage>>* best_vec = nullptr;
            size_t                                best_i   = 0;
            long                                  best_use = 0;
            for(auto& kv : images)
            {
                auto& vars = kv.second->vars;
                for(size_t i = 0; i < vars.size(); i++)
                    if(vars[i]->last_y <= y1 && (!best_vec || vars[i]->last_use < best_use))
                    {
                        best_vec = &vars;
                        best_i   = i;
                        best_use = vars[i]->last_use;
                    }
            }
            if(!best_vec) break;
            img_bytes -= (*best_vec)[best_i]->bytes();
            best_vec->erase(best_vec->begin() + static_cast<long>(best_i));
        }
    }

    // nearest-neighbour scaled blit of img into dst rect, clipped
    void blit(const gimage& img, const rect_i& dst, const rect_i& clip0)
    {
        rect_i clip = intersect(intersect(clip0, cur_clip()), dst);
        int    dw = dst.x1 - dst.x0, dh = dst.y1 - dst.y0;
        if(dw <= 0 || dh <= 0 || img.w <= 0 || img.h <= 0) return;
        for(int y = clip.y0; y < clip.y1; y++)
        {
            int sy = static_cast<int>(static_cast<long>(y - dst.y0) * img.h / dh);
            for(int x = clip.x0; x < clip.x1; x++)
            {
                int    sx = static_cast<int>(static_cast<long>(x - dst.x0) * img.w / dw);
                size_t o  = static_cast<size_t>(sy) * img.w + sx;
                if(img.opaque_at(o)) blend(x, y, {img.px[3 * o], img.px[3 * o + 1], img.px[3 * o + 2]}, 255);
            }
        }
    }

    void draw_image(uint_ptr, const background_layer& layer, const std::string& url, const std::string& base_url) override
    {
        img_src* s = find_image(url.c_str(), base_url.c_str());
        if(!s) return;
        rect_i clip = intersect(to_rect(layer.clip_box), to_rect(layer.border_box));
        rect_i o    = to_rect(layer.origin_box);
        int    tw = o.x1 - o.x0, th = o.y1 - o.y0;
        if(tw <= 0 || th <= 0) return;
        bool rx = layer.repeat == background_repeat_repeat || layer.repeat == background_repeat_repeat_x;
        bool ry = layer.repeat == background_repeat_repeat || layer.repeat == background_repeat_repeat_y;
        // nothing of this layer is inside the current band: do not fetch or decode
        rect_i need = intersect(clip, cur_clip());
        if(!rx) need.x0 = std::max(need.x0, o.x0), need.x1 = std::min(need.x1, o.x1);
        if(!ry) need.y0 = std::max(need.y0, o.y0), need.y1 = std::min(need.y1, o.y1);
        if(need.empty()) return;
        int bottom = std::min(clip.y1, ry ? clip.y1 : o.y1);
        if(collect)
        {
            want(*s, bottom);
            return;
        }
        gimage* img = variant(*s, tw, th, bottom);
        if(!img) return;
        int x_start = o.x0, y_start = o.y0;
        if(rx) while(x_start > clip.x0) x_start -= tw;
        if(ry) while(y_start > clip.y0) y_start -= th;
        for(int ty = y_start; ty < (ry ? clip.y1 : y_start + 1); ty += th)
            for(int tx = x_start; tx < (rx ? clip.x1 : x_start + 1); tx += tw)
                blit(*img, {tx, ty, tx + tw, ty + th}, clip);
    }

    void draw_solid_fill(uint_ptr, const background_layer& layer, const web_color& color) override
    {
        if(color.alpha == 0) return;
        rect_i r = intersect(to_rect(layer.clip_box), to_rect(layer.border_box));
        if(rounded(layer.border_radius))
        {
            // the radii belong to the border box; the clip box only limits the rows and columns
            rect_i saved_clip = cur_clip();
            clips.push_back(intersect(saved_clip, r));
            fill_rounded(to_rect(layer.border_box), layer.border_radius, to_rgb(color), color.alpha);
            clips.pop_back();
            return;
        }
        fill(r, to_rgb(color), color.alpha);
    }

    // gradients approximated by the mean of the colour stops
    void grad_fill(const background_layer& layer, const background_layer::gradient_base& g)
    {
        if(g.color_points.empty()) return;
        int r = 0, gr = 0, b = 0, a = 0, n = static_cast<int>(g.color_points.size());
        for(auto& p : g.color_points)
        {
            r += p.color.red;
            gr += p.color.green;
            b += p.color.blue;
            a += p.color.alpha;
        }
        rgb v{static_cast<uint8_t>(r / n), static_cast<uint8_t>(gr / n), static_cast<uint8_t>(b / n)};
        fill(intersect(to_rect(layer.clip_box), to_rect(layer.border_box)), v, a / n);
    }
    void draw_linear_gradient(uint_ptr, const background_layer& l, const background_layer::linear_gradient& g) override
    {
        grad_fill(l, g);
    }
    void draw_radial_gradient(uint_ptr, const background_layer& l, const background_layer::radial_gradient& g) override
    {
        grad_fill(l, g);
    }
    void draw_conic_gradient(uint_ptr, const background_layer& l, const background_layer::conic_gradient& g) override
    {
        grad_fill(l, g);
    }

    static bool visible(const border& b)
    {
        return b.width > 0_px && b.style != border_style_none && b.style != border_style_hidden && b.color.alpha;
    }

    void draw_borders(uint_ptr, const borders& b, const position& p, bool) override
    {
        rect_i r  = to_rect(p);
        int    l  = visible(b.left) ? static_cast<int>(std::lround(b.left.width.value())) : 0;
        int    t  = visible(b.top) ? static_cast<int>(std::lround(b.top.width.value())) : 0;
        int    rt = visible(b.right) ? static_cast<int>(std::lround(b.right.width.value())) : 0;
        int    bt = visible(b.bottom) ? static_cast<int>(std::lround(b.bottom.width.value())) : 0;
        if(rounded(b.radius))
        {
            // Ring between the outer rounded box and the inner one inset by the border widths, in the
            // colour of the first visible side. Rows without a visible top or bottom border keep the
            // inner box open on that side.
            const border& side = visible(b.top) ? b.top : visible(b.left) ? b.left : visible(b.right) ? b.right : b.bottom;
            if(!visible(side)) return;
            rgb    v     = to_rgb(side.color);
            int    a     = side.color.alpha;
            rect_i inner = {r.x0 + l, r.y0 + t, r.x1 - rt, r.y1 - bt};
            border_radiuses ir = b.radius;
            auto shrink = [](pixel_t& v, int d) { v = std::max<pixel_t>(0_px, v - pixel_t(static_cast<float>(d))); };
            shrink(ir.top_left_x, l), shrink(ir.top_left_y, t), shrink(ir.top_right_x, rt), shrink(ir.top_right_y, t);
            shrink(ir.bottom_left_x, l), shrink(ir.bottom_left_y, bt), shrink(ir.bottom_right_x, rt), shrink(ir.bottom_right_y, bt);
            rect_i clip = intersect(r, cur_clip());
            for(int y = clip.y0; y < clip.y1; y++)
            {
                int ox0, ox1, ix0, ix1;
                if(!rounded_span(r, b.radius, y, ox0, ox1)) continue;
                bool has_inner = rounded_span(inner, ir, y, ix0, ix1);
                int  xa = std::max(ox0, clip.x0), xb = std::min(ox1, clip.x1);
                for(int x = xa; x < xb; x++)
                    if(!has_inner || x < ix0 || x >= ix1) blend(x, y, v, a);
            }
            return;
        }
        // solid only; dashed and dotted drawn as solid
        if(visible(b.top)) fill({r.x0, r.y0, r.x1, r.y0 + std::max(1, t)}, to_rgb(b.top.color), b.top.color.alpha);
        if(visible(b.bottom))
            fill({r.x0, r.y1 - std::max(1, bt), r.x1, r.y1}, to_rgb(b.bottom.color), b.bottom.color.alpha);
        if(visible(b.left))
            fill({r.x0, r.y0, r.x0 + std::max(1, l), r.y1}, to_rgb(b.left.color), b.left.color.alpha);
        if(visible(b.right))
            fill({r.x1 - std::max(1, rt), r.y0, r.x1, r.y1}, to_rgb(b.right.color), b.right.color.alpha);
    }

    void set_caption(const char* text) override
    {
        // collapse whitespace, the tab shows one line
        caption.clear();
        bool space = true;
        for(const char* p = text ? text : ""; *p; p++)
        {
            bool ws = *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t';
            if(ws && space) continue;
            caption += ws ? ' ' : *p;
            space = ws;
        }
        while(!caption.empty() && caption.back() == ' ') caption.pop_back();
    }
    void set_base_url(const char* url) override
    {
        std::string b = resolve_url(page_url, url ? url : "");
        if(!b.empty()) base_url = b;
    }
    // Every <link> that is not a stylesheet. Icon candidates are scored: a plain icon of 16 or 32
    // pixels in a raster format first, apple-touch-icon last, SVG never.
    void link(const std::shared_ptr<document>&, const element::ptr& el) override
    {
        std::string rel  = el->get_attr("rel", "");
        const char* href = el->get_attr("href", "");
        std::transform(rel.begin(), rel.end(), rel.begin(), ::tolower);
        if(!href || !*href || rel.find("icon") == std::string::npos) return;
        std::string type  = el->get_attr("type", "");
        std::string sizes = el->get_attr("sizes", "");
        std::string h     = href;
        auto        q     = h.find('?');
        std::string path  = q == std::string::npos ? h : h.substr(0, q);
        if(type.find("svg") != std::string::npos || (path.size() > 4 && path.compare(path.size() - 4, 4, ".svg") == 0))
            return;
        int score = 50;
        if(rel.find("apple-touch") != std::string::npos) score = 90;
        if(sizes.find("16x16") != std::string::npos) score -= 20;
        else if(sizes.find("32x32") != std::string::npos) score -= 15;
        else if(!sizes.empty() && sizes != "any") score += 10;
        if(type.find("png") != std::string::npos) score -= 5;
        icons.push_back({resolve(href, nullptr), score});
    }
    void on_anchor_click(const char*, const element::ptr&) override {}
    void on_mouse_event(const element::ptr&, mouse_event) override {}
    void set_cursor(const char*) override {}

    void transform_text(std::string& text, text_transform tt) override
    {
        // ASCII only
        if(tt == text_transform_uppercase)
            for(auto& c : text) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        else if(tt == text_transform_lowercase)
            for(auto& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        else if(tt == text_transform_capitalize && !text.empty())
            text[0] = static_cast<char>(toupper(static_cast<unsigned char>(text[0])));
    }

    // Called during parse for every <link rel=stylesheet> and @import. baseurl becomes the stylesheet
    // URL so relative references inside it resolve against it.
    void import_css(std::string& text, const std::string& url, std::string& baseurl) override
    {
        std::string abs = resolve(url.c_str(), baseurl.c_str());
        if(abs.empty() || abs.compare(0, 5, "data:") == 0) return;
        if(!fetch_url(abs, text, CSS_MAX)) text.clear();
        note_psram();
        baseurl = abs;
    }

    void set_clip(const position& pos, const border_radiuses&) override
    {
        clips.push_back(intersect(cur_clip(), to_rect(pos)));
    }
    void del_clip() override
    {
        if(!clips.empty()) clips.pop_back();
    }

    void get_viewport(position& vp) const override
    {
        vp.x      = 0;
        vp.y      = 0;
        vp.width  = VIEW_W;
        vp.height = view_h;
    }

    element::ptr create_element(const char*, const string_map&, const std::shared_ptr<document>&) override
    {
        return nullptr;
    }

    void get_media_features(media_features& m) const override
    {
        m.type          = media_type_screen;
        m.width         = VIEW_W;
        m.height        = view_h;
        m.device_width  = VIEW_W;
        m.device_height = view_h;
        // the same features as the reference renders in browsers/litehtml/out/lanes/device/bwr
        m.color         = 0;
        m.color_index   = 0;
        m.monochrome    = 8;
        m.resolution    = 96;
    }

    void get_language(std::string& lang, std::string& culture) const override
    {
        lang    = "en";
        culture = "";
    }
};

// ---------------------------------------------------------------- pipeline

static face_set g_fonts;

bool render_init()
{
    fetch_init();
    return g_fonts.init();
}

static int ms_since(int64_t t0) { return static_cast<int>((esp_timer_get_time() - t0) / 1000); }

// A page whose body is an image file: one <img> scaled to fit the window, aspect kept, centred, with a
// black border of IMAGE_BORDER px. The title is the last path segment.
static const int IMAGE_BORDER = 3;

static std::string image_page_html(const std::string& url, int nat_w, int nat_h)
{
    int box_w = VIEW_W - 2 * IMAGE_BORDER, box_h = PAGE_H - 2 * IMAGE_BORDER;
    int w = box_w, h = static_cast<int>(static_cast<int64_t>(nat_h) * box_w / nat_w);
    if(h > box_h)
    {
        h = box_h;
        w = static_cast<int>(static_cast<int64_t>(nat_w) * box_h / nat_h);
    }
    w = std::max(1, w), h = std::max(1, h);
    int left = (VIEW_W - w - 2 * IMAGE_BORDER) / 2, top = (PAGE_H - h - 2 * IMAGE_BORDER) / 2;

    std::string title = url;
    auto        q     = title.find_first_of("?#");
    if(q != std::string::npos) title.erase(q);
    while(!title.empty() && title.back() == '/') title.pop_back();
    auto slash = title.rfind('/');
    if(slash != std::string::npos && slash + 1 < title.size()) title.erase(0, slash + 1);

    char img[192];
    snprintf(img, sizeof(img),
             "<img width=\"%d\" height=\"%d\" style=\"display:block;margin:%dpx 0 0 %dpx;border:%dpx solid #000\" src=\"",
             w, h, top, left, IMAGE_BORDER);
    return "<html><head><title>" + html_escape(title) + "</title></head><body style=\"margin:0;padding:0;background:#fff\">" +
           img + html_escape(url) + "\"></body></html>";
}

// Title and favicon for the tab bar, stored per page URL. The icon is fetched only when none is
// stored yet: the best <link rel=icon> candidate, else /favicon.ico at the origin. A failed icon fetch
// leaves the entry without an icon and is tried again on the next load of the page.
static const size_t ICON_FILE_MAX = 64 * 1024;

static void update_tab(bwr_container& c)
{
    tab_info t;
    bool     had   = tabs_load(c.page_url, t);
    std::string title = c.caption.empty() ? tabs_host(c.page_url) : c.caption;
    bool     changed = !had || t.title != title;
    t.title          = title;
    if(t.icon_bmp.empty())
    {
        std::vector<std::string> urls;
        std::sort(c.icons.begin(), c.icons.end(), [](const bwr_container::icon_cand& a, const bwr_container::icon_cand& b) {
            return a.score < b.score;
        });
        for(auto& ic : c.icons)
            if(!ic.href.empty() && urls.size() < 2) urls.push_back(ic.href);
        std::string origin = c.page_url;
        auto        p      = origin.find("://");
        auto        q      = p == std::string::npos ? std::string::npos : origin.find('/', p + 3);
        if(q != std::string::npos) origin = origin.substr(0, q);
        urls.push_back(origin + "/favicon.ico");
        for(auto& u : urls)
        {
            std::string  file;
            fetch_result r;
            if(!fetch_url(u, file, ICON_FILE_MAX, &r)) continue;
            t.icon_bmp = tabs_icon_bmp(file);
            if(!t.icon_bmp.empty())
            {
                ESP_LOGI(TAG, "favicon %s: %u bytes", u.c_str(), (unsigned)file.size());
                changed = true;
                break;
            }
            ESP_LOGW(TAG, "favicon %s: %u bytes, not decodable", u.c_str(), (unsigned)file.size());
        }
    }
    if(changed) tabs_save(c.page_url, t);
}

bool render_tabs(const std::vector<page_entry>& pages, size_t current, uint8_t* plane_bw, uint8_t* plane_red)
{
    std::vector<tab_info> infos(pages.size());
    for(size_t i = 0; i < pages.size(); i++) tabs_load(pages[i].url, infos[i]);
    std::string html = tabs_html(pages, current, infos, TAB_H);
    bool        ok   = false;
    {
        bwr_container c(g_fonts, "tabs:", TAB_H);
        auto doc = document::createFromString(html, &c, litehtml::master_css, {}, static_release_after_cascade);
        if(!doc) return false;
        c.phase = 1;
        doc->render(VIEW_W);
        c.phase = 3;
        if(c.alloc_window(TAB_H))
        {
            c.begin_band(0, TAB_H, TAB_H);
            position clip(0, 0, VIEW_W, TAB_H);
            doc->draw(0, 0, 0, &clip);
            c.end_band(TAB_H);
            for(int y = 0; y < TAB_H; y++)
                c.q.row(&c.fb[static_cast<size_t>(y) * VIEW_W * 3], y, plane_bw + static_cast<size_t>(y) * EPD_STRIDE,
                        plane_red + static_cast<size_t>(y) * EPD_STRIDE);
            ok = true;
        }
        c.free_window();
        doc.reset();
    }
    reset_string_ids();
    return ok;
}

bool render_page(const std::string& url, int offset, uint8_t* plane_bw, uint8_t* plane_red, render_stats& st,
                 std::string& err)
{
    st = render_stats();
    memset(plane_bw, 0, EPD_PLANE_BYTES);
    memset(plane_red, 0, EPD_PLANE_BYTES);
    bool ok = false;
    {
        bwr_container c(g_fonts, url, PAGE_H);
        c.st = &st;

        int64_t     t0 = esp_timer_get_time();
        std::string html;
        fetch_result fr;
        bool got = fetch_url(url, html, PAGE_HTML_MAX, &fr);
        st.fetch_ms = ms_since(t0);
        if(!got)
        {
            char buf[96];
            if(fr.truncated) snprintf(buf, sizeof(buf), "page larger than %u KB", (unsigned)(PAGE_HTML_MAX / 1024));
            else if(fr.status) snprintf(buf, sizeof(buf), "HTTP %d", fr.status);
            else snprintf(buf, sizeof(buf), "fetch failed");
            err = buf;
            return false;
        }
        st.html_bytes = html.size();
        c.page_url = c.base_url = fr.final_url.empty() ? url : fr.final_url;
        leds_load(2, 8);

        // An image file as the page: the fetched bytes become the one loaded image of a wrapper page,
        // keyed as load_image will key its src, so nothing is fetched again and no per-image cap applies.
        {
            int iw = 0, ih = 0;
            int kind = html.size() > 8 ? sniff_size(html, iw, ih) : IMG_NONE;
            if(kind != IMG_NONE && iw > 0 && ih > 0)
            {
                std::string key = resolve_url(c.base_url, c.base_url);
                auto        s   = std::make_unique<bwr_container::img_src>();
                s->kind         = kind;
                s->nat_w        = iw;
                s->nat_h        = ih;
                s->state        = bwr_container::IMG_LOADED;
                s->data.swap(html);
                shrink_to_fit(s->data);
                c.img_src_bytes += s->data.size();
                c.img_count++;
                c.images[key] = std::move(s);
                html = image_page_html(key, iw, ih);
                ESP_LOGI(TAG, "image page: kind %d, %dx%d, %u bytes", kind, iw, ih, (unsigned)st.html_bytes);
            }
        }

        t0 = esp_timer_get_time();
        auto doc = document::createFromString(html, &c, litehtml::master_css, USER_CSS, static_release_after_cascade);
        std::string().swap(html); // the engine keeps no copy of the source
        st.parse_ms = ms_since(t0);
        if(!doc)
        {
            err = "refused: not enough memory for this page";
            reset_string_ids();
            return false;
        }

        leds_load(3, 8);
        c.phase = 1;
        t0      = esp_timer_get_time();
        doc->render(VIEW_W);
        st.layout_ms = ms_since(t0);
        leds_load(4, 8);
        st.doc_h = static_cast<int>(std::ceil(doc->height().value()));
        // keep the window inside the document; a short document shows from its top
        offset    = std::max(0, std::min(offset, st.doc_h - PAGE_H));
        st.offset = offset;
        c.note_psram();

        // Collect pass: one draw over the whole window with painting off, so the container learns which
        // images the window needs and how far down. Then those files are fetched. A file whose size
        // differs from what layout used triggers one more layout and collect.
        t0 = esp_timer_get_time();
        for(int pass = 0; pass < 2; pass++)
        {
            c.phase   = 2;
            c.collect = true;
            c.begin_band(0, PAGE_H, PAGE_H);
            position whole(0, 0, VIEW_W, PAGE_H);
            doc->draw(0, 0, static_cast<pixel_t>(-offset), &whole);
            c.collect       = false;
            c.size_mismatch = false;
            for(auto& kv : c.images)
                if(kv.second->state == bwr_container::IMG_WANTED) c.load_full(kv.first, *kv.second);
            if(!c.size_mismatch) break;
            st.relayout = true;
            c.phase     = 1; // layout again: deferred sizes stay 0 here too
            doc->render(VIEW_W);
            st.doc_h  = static_cast<int>(std::ceil(doc->height().value()));
            offset    = std::max(0, std::min(offset, st.doc_h - PAGE_H));
            st.offset = offset;
        }
        for(auto& kv : c.images)
            if(kv.second->state == bwr_container::IMG_LOADED) st.img_in_view++;
        st.images_ms = ms_since(t0);
        leds_load(5, 8);
        c.note_psram();

        t0      = esp_timer_get_time();
        c.phase = 3;
        if(!c.alloc_window(BAND_ROWS))
        {
            err = "no memory for the band buffer";
        } else
        {
            for(int y0 = 0; y0 < PAGE_H; y0 += BAND_ROWS)
            {
                int y1 = std::min(PAGE_H, y0 + BAND_ROWS);
                c.begin_band(y0, y1, PAGE_H);
                position clip(0, y0, VIEW_W, y1 - y0);
                doc->draw(0, 0, static_cast<pixel_t>(-offset), &clip);
                c.end_band(y1);
                // page row y lands on panel row TAB_H + y
                for(int y = y0; y < y1; y++)
                    c.q.row(&c.fb[static_cast<size_t>(y - y0) * VIEW_W * 3], TAB_H + y,
                            plane_bw + static_cast<size_t>(TAB_H + y) * EPD_STRIDE,
                            plane_red + static_cast<size_t>(TAB_H + y) * EPD_STRIDE);
                leds_load(5 + (y1 * 3 + PAGE_H - 1) / PAGE_H, 8); // draw fills the last three LEDs
            }
            ok = true;
        }
        c.free_window();
        st.draw_ms  = ms_since(t0);
        st.images   = c.img_count;
        st.img_fail = c.img_fail;
        if(ok) update_tab(c);
        ESP_LOGI(TAG, "fonts %ld, text_width %ld, draw_text %ld, glyph cache %u bytes peak, %ld evictions",
                 c.n_fonts, c.n_measure, c.n_draw_text, (unsigned)c.gcache.peak, c.gcache.evictions);
        ESP_LOGI(TAG,
                 "images: %u refs, %u probes, %u cache hits, %u in view, %u bytes fetched, "
                 "%u skipped, %u failed, relayout %d, cache %d entries",
                 (unsigned)st.images, (unsigned)st.img_probes, (unsigned)st.img_cache_hits, (unsigned)st.img_in_view,
                 (unsigned)st.img_fetched_bytes, (unsigned)st.img_skipped, (unsigned)st.img_fail, st.relayout ? 1 : 0,
                 sizecache_count());
        doc.reset();
    }
    // the id table grows across pages without this
    reset_string_ids();
    sizecache_save();
    return ok;
}
