// Tone rendering. See tones.h.

#include "tones.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "epd97.h"
#include "tonemap.h"

static const char* TAG = "tones";

// The seq unit the tone map was measured at, and the erase that precedes every draw (139 frames at unit 4, measured 5.8 s with the supply cut).
static const uint8_t TONES_UNIT    = 4;
static const char*   TONES_ERASE   = "iiiippppiiiippppiippiippipiippppp";
// Carried error limits in Oklab units. L spans 0.23 (black) to 1.0 (paper). The widest gap between
// neutral entries is 0.14 in L, so the L limit must pass that. Chroma error is held tighter so a
// run of pinks cannot build up into a red dot on a light area.
static const float ERR_CLAMP_L = 0.25f;
static const float ERR_CLAMP_C = 0.08f;
// Candidates searched per coloured pixel: the entry the mapping table gives for the pixel without
// error, and its nearest neighbours in the weighted Oklab distance. A host model of the table found
// misses against a full search at 12 and 16 candidates on light reds, none at 24. Gray pixels search
// the neutral entries directly.
static const int NB = 24;
// An error below this in every axis is dropped: a paper pixel then costs no search or diffusion.
static const float ERR_ZERO = 1e-3f;
// A pixel whose channels differ by more than this is a colour, not a gray, for the lightness rule.
static const int CHROMA_MIN = 8;
// Mapping table resolution: ink and red in 4-unit cells. TONEMAP_GRAY_MAX_RED is a multiple of 4,
// so the gray rule is exact per cell.
static const int LUT_N = 64;

// Dither settings. The host harness builds each combination with -D overrides and compares them
// (host_tests/tones/README.md).
// Kernel: KERNEL_ATKINSON (6 taps, diffuses 6/8 of the error), KERNEL_FLOYD_STEINBERG (4 taps),
// KERNEL_JJN or KERNEL_STUCKI (12 taps). All fit the three error rows.
// Atkinson is the page default: it keeps text and graphics edges solid (host test: 509 near-edge
// white pixels off paper against 1709 for Stucki with the same noise). The full-error kernels are
// for photo regions, where its dropped quarter of the error shows as steps in dark gradients.
#ifndef TONES_DITHER_KERNEL
#define TONES_DITHER_KERNEL KERNEL_ATKINSON
#endif
// Threshold noise amplitude in Oklab L: the search point's L moves by up to this much from a 64x64
// table. The carried error is formed without it, so the noise moves dots and not the mean tone.
// 0 turns it off.
#ifndef TONES_DITHER_NOISE_L
#define TONES_DITHER_NOISE_L 0.0f
#endif
// Threshold noise as a fraction of the gap between the two nearest entries. The decision point
// between them moves off their midpoint by up to this fraction of the gap, toward either entry by
// the noise table. Below 0.5 a pixel on an entry stays on it. 0 turns it off.
#ifndef TONES_DITHER_NOISE_REL
#define TONES_DITHER_NOISE_REL 0.25f
#endif
// 1: blue noise table (void and cluster ranking). 0: white noise from the hash.
#ifndef TONES_DITHER_NOISE_BLUE
#define TONES_DITHER_NOISE_BLUE 1
#endif
// A pixel whose search point lies within this weighted Oklab distance of the chosen entry diffuses
// by Atkinson, which drops a quarter of the error. Others use the kernel above. 0 turns it off.
#ifndef TONES_DITHER_HYBRID_NEAR
#define TONES_DITHER_HYBRID_NEAR 0.0f
#endif
// Share of a coloured pixel's chroma residual that is diffused. Near black the target's b lies
// above every dark entry's b, so chroma error diffused in full builds up to ERR_CLAMP_C and pulls
// the picks to lighter reds. On the harness dark red ramp the band L then runs up to 0.029 above
// the target with a full-error kernel, 0.009 at 0.5.
#ifndef TONES_ERR_KEEP_C
#define TONES_ERR_KEEP_C 1.0f
#endif
static const int NOISE_N = 64;

namespace
{
const unsigned N = static_cast<unsigned>(TONEMAP_COUNT);

enum dither_kernel
{
    KERNEL_ATKINSON,
    KERNEL_FLOYD_STEINBERG,
    KERNEL_JJN,
    KERNEL_STUCKI,
};

struct dither_tap
{
    int8_t dx, dy; // dx along the scan direction, dy rows down
    float  w;
};

struct dither_taps
{
    int        n;
    dither_tap t[12];
};

// Indexed by dither_kernel. Every tap stays within 2 columns and 2 rows, the reach of the error
// rows in tone_dither.
const dither_taps k_kernels[] = {
    {6, {{1, 0, 1 / 8.0f}, {2, 0, 1 / 8.0f}, {-1, 1, 1 / 8.0f}, {0, 1, 1 / 8.0f}, {1, 1, 1 / 8.0f}, {0, 2, 1 / 8.0f}}},
    {4, {{1, 0, 7 / 16.0f}, {-1, 1, 3 / 16.0f}, {0, 1, 5 / 16.0f}, {1, 1, 1 / 16.0f}}},
    {12, {{1, 0, 7 / 48.0f}, {2, 0, 5 / 48.0f}, {-2, 1, 3 / 48.0f}, {-1, 1, 5 / 48.0f}, {0, 1, 7 / 48.0f},
          {1, 1, 5 / 48.0f}, {2, 1, 3 / 48.0f}, {-2, 2, 1 / 48.0f}, {-1, 2, 3 / 48.0f}, {0, 2, 5 / 48.0f},
          {1, 2, 3 / 48.0f}, {2, 2, 1 / 48.0f}}},
    {12, {{1, 0, 8 / 42.0f}, {2, 0, 4 / 42.0f}, {-2, 1, 2 / 42.0f}, {-1, 1, 4 / 42.0f}, {0, 1, 8 / 42.0f},
          {1, 1, 4 / 42.0f}, {2, 1, 2 / 42.0f}, {-2, 2, 1 / 42.0f}, {-1, 2, 2 / 42.0f}, {0, 2, 4 / 42.0f},
          {1, 2, 2 / 42.0f}, {2, 2, 1 / 42.0f}}},
};

const dither_taps& KERNEL       = k_kernels[TONES_DITHER_KERNEL];
const float        NOISE_STEP   = TONES_DITHER_NOISE_L / 128.0f; // per unit of s_noise
const float        NOISE_REL    = TONES_DITHER_NOISE_REL / 64.0f; // per unit of s_noise, doubled
const float        HYBRID_NEAR2 = TONES_DITHER_HYBRID_NEAR * TONES_DITHER_HYBRID_NEAR;

static_assert(static_cast<size_t>(TONE_W) * TONE_H == static_cast<size_t>(EPD_PLANE_BYTES) * 8, "frame and planes differ");
static_assert(TONE_W == EPD_W && TONE_H == EPD_H, "frame and panel differ");

float    s_lab[TONEMAP_COUNT][3];
bool     s_neutral[TONEMAP_COUNT];
uint8_t  s_nb[TONEMAP_COUNT][NB];
uint8_t  s_neutrals[TONEMAP_COUNT]; // the neutral entries, for gray pixels
int      s_n_neutral = 0;
uint8_t  s_lut[LUT_N][LUT_N];
uint8_t  s_lift[256];
float    s_lin[256];                // sRGB byte to linear
float    s_cw[3], s_cb[3], s_cr[3]; // paper, black and red corners in Oklab
float    s_l_min, s_l_max;
uint8_t  s_black = 0;
uint8_t* s_frame = nullptr;
bool     s_seq_ok = true; // false once the driver reported no seq tables
int8_t   s_noise[NOISE_N][NOISE_N]; // threshold noise, -128 to 127, uniform

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Blue: ranks the cells of s_noise. Each rank goes to the cell of least energy, the energy being a
// Gaussian (sigma 1.5, radius 5, wrapped) around every cell ranked so far, with hash values below
// 1e-6 to break ties. Row minima are kept so a rank costs one splat and 11 row scans.
// White: the hash of the cell index.
void build_noise()
{
    const int R = 5, NN = NOISE_N * NOISE_N;
    if(!TONES_DITHER_NOISE_BLUE)
    {
        for(int i = 0; i < NN; i++)
        {
            const int h = static_cast<int>(hash32(static_cast<uint32_t>(i)) >> 24);
            s_noise[i / NOISE_N][i % NOISE_N] = static_cast<int8_t>(h - 128);
        }
        return;
    }
    float g[2 * R + 1][2 * R + 1];
    for(int dy = -R; dy <= R; dy++)
        for(int dx = -R; dx <= R; dx++) g[dy + R][dx + R] = expf(-(dx * dx + dy * dy) / 4.5f);
    std::vector<float> en(NN);
    for(int i = 0; i < NN; i++) en[i] = (hash32(static_cast<uint32_t>(i)) >> 8) * (1e-6f / (1 << 24));
    int  arg[NOISE_N];
    auto scan = [&](int y) {
        arg[y] = 0;
        for(int x = 1; x < NOISE_N; x++)
            if(en[y * NOISE_N + x] < en[y * NOISE_N + arg[y]]) arg[y] = x;
    };
    for(int y = 0; y < NOISE_N; y++) scan(y);
    for(int r = 0; r < NN; r++)
    {
        int y = 0;
        for(int k = 1; k < NOISE_N; k++)
            if(en[k * NOISE_N + arg[k]] < en[y * NOISE_N + arg[y]]) y = k;
        const int x         = arg[y];
        s_noise[y][x]       = static_cast<int8_t>(r * 256 / NN - 128);
        en[y * NOISE_N + x] = 1e9f;
        for(int dy = -R; dy <= R; dy++)
            for(int dx = -R; dx <= R; dx++)
                en[((y + dy) & (NOISE_N - 1)) * NOISE_N + ((x + dx) & (NOISE_N - 1))] += g[dy + R][dx + R];
        for(int dy = -R; dy <= R; dy++) scan((y + dy) & (NOISE_N - 1));
    }
}

void oklab(uint8_t r8, uint8_t g8, uint8_t b8, float out[3])
{
    float c[3] = {r8 / 255.0f, g8 / 255.0f, b8 / 255.0f};
    for(float& v : c) v = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

// Oklab lightness of an sRGB pixel through the linear table.
inline float pixel_L(int r8, int g8, int b8)
{
    float r = s_lin[r8], g = s_lin[g8], b = s_lin[b8];
    const float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
}

int entry_of(const char* pattern)
{
    for(unsigned e = 0; e < N; e++)
        if(strcmp(k_tonemap[e].pattern, pattern) == 0) return static_cast<int>(e);
    return -1;
}

inline float dist(const float* a, const float* b)
{
    float dl = a[0] - b[0];
    float da = (a[1] - b[1]) * TONEMAP_CIE_CHROMA_WEIGHT;
    float db = (a[2] - b[2]) * TONEMAP_CIE_CHROMA_WEIGHT;
    return dl * dl + da * da + db * db;
}

// Oklab target of an (ink, red) pair after the tone curve. ink + red <= 255.
inline void target(int ink, int red, float t[3])
{
    int w = 255 - ink - red;
    for(int i = 0; i < 3; i++) t[i] = (w * s_cw[i] + ink * s_cb[i] + red * s_cr[i]) * (1.0f / 255.0f);
}

// Nearest entry to t over all entries, neutral ones only when gray.
unsigned nearest_all(const float t[3], bool gray)
{
    unsigned best = 0;
    float    bd   = 1e9f;
    for(unsigned e = 0; e < N; e++)
    {
        if(gray && !s_neutral[e]) continue;
        float d = dist(t, s_lab[e]);
        if(d < bd) bd = d, best = e;
    }
    return best;
}

// Seq table of entry e at step k: 0 hold, 1 paper, 2 ink, 3 red. Hold past the pattern's end.
inline int table_at(unsigned e, unsigned k)
{
    const char* p = k_tonemap[e].pattern;
    if(k >= strlen(p)) return 0;
    switch(p[k])
    {
    case 'p': return 1;
    case 'i': return 2;
    case 'r': return 3;
    default: return 0;
    }
}

void build_planes(const uint8_t* frame, const uint8_t tab[TONEMAP_COUNT])
{
    uint8_t* bw  = epd_plane_bw();
    uint8_t* red = epd_plane_red();
    for(size_t i = 0; i < EPD_PLANE_BYTES; i++)
    {
        const uint8_t* px = frame + i * 8;
        uint8_t        b = 0, r = 0;
        for(unsigned bit = 0; bit < 8; bit++)
        {
            int t = tab[px[bit]];
            if(t & 1) b |= static_cast<uint8_t>(0x80u >> bit);
            if(t & 2) r |= static_cast<uint8_t>(0x80u >> bit);
        }
        bw[i]  = b;
        red[i] = r;
    }
}

// The three stock colours from the entries' measured ink and red, for a panel without seq tables.
// Stock red is the red bit alone (plane pair 2), stock black the bw bit alone (pair 1).
bool draw_stock(const uint8_t* frame)
{
    uint8_t tab[TONEMAP_COUNT];
    for(unsigned e = 0; e < N; e++)
        tab[e] = k_tonemap[e].red >= TONEMAP_REF_RED ? 2 : k_tonemap[e].ink >= TONEMAP_REF_INK ? 1 : 0;
    build_planes(frame, tab);
    return refresh_display(EPD_SLOW);
}
} // namespace

bool tones_init()
{
    s_n_neutral = 0;
    for(unsigned e = 0; e < N; e++)
    {
        oklab(k_tonemap[e].r, k_tonemap[e].g, k_tonemap[e].b, s_lab[e]);
        s_neutral[e] = s_lab[e][1] <= TONEMAP_GRAY_A_MAX;
        if(s_neutral[e]) s_neutrals[s_n_neutral++] = static_cast<uint8_t>(e);
    }
    // lightest first, so a gray's bracketing pair is found by one scan
    std::sort(s_neutrals, s_neutrals + s_n_neutral, [](uint8_t a, uint8_t b) { return s_lab[a][0] > s_lab[b][0]; });
    int eb = entry_of("iiiii"), er = entry_of(TONEMAP_CIE_RED_PATTERN);
    if(eb < 0 || er < 0 || entry_of("") != TONE_PAPER)
    {
        ESP_LOGE(TAG, "tone map lacks a corner entry, or paper is not entry %d", TONE_PAPER);
        return false;
    }
    s_black = static_cast<uint8_t>(eb);
    memcpy(s_cw, s_lab[TONE_PAPER], sizeof(s_cw));
    memcpy(s_cb, s_lab[eb], sizeof(s_cb));
    memcpy(s_cr, s_lab[er], sizeof(s_cr));
    s_l_min = s_cb[0];
    s_l_max = s_cw[0];
    for(int v = 0; v < 256; v++)
    {
        s_lift[v] = static_cast<uint8_t>(255.0f * powf(v / 255.0f, TONEMAP_GAMMA) + 0.5f);
        float c   = v / 255.0f;
        s_lin[v]  = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }

    for(int i = 0; i < LUT_N; i++)
        for(int r = 0; r < LUT_N; r++)
        {
            int   ink = std::min(255, i * 4 + 2), red = std::min(255, r * 4 + 2);
            if(ink + red > 255) red = 255 - ink;
            float t[3];
            target(ink, red, t);
            s_lut[i][r] = static_cast<uint8_t>(nearest_all(t, red < TONEMAP_GRAY_MAX_RED));
        }

    for(unsigned e = 0; e < N; e++)
    {
        // the NB nearest entries to e, e itself first
        std::vector<std::pair<float, unsigned>> d;
        d.reserve(N);
        for(unsigned f = 0; f < N; f++) d.push_back({dist(s_lab[e], s_lab[f]), f});
        std::sort(d.begin(), d.end());
        for(int k = 0; k < NB; k++) s_nb[e][k] = static_cast<uint8_t>(d[k].second);
    }
    build_noise();

    s_frame = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(TONE_W) * TONE_H, MALLOC_CAP_SPIRAM));
    if(!s_frame)
    {
        ESP_LOGE(TAG, "no PSRAM for the frame");
        return false;
    }
    memset(s_frame, TONE_PAPER, static_cast<size_t>(TONE_W) * TONE_H);
    ESP_LOGI(TAG, "%u entries, paper L %.2f, black L %.2f, red L %.2f a %.2f, gamma %.2f", N, s_cw[0], s_cb[0],
             s_cr[0], s_cr[1], static_cast<double>(TONEMAP_GAMMA));
    return true;
}

uint8_t tones_black()
{
    return s_black;
}

int tones_solid_gray(int g)
{
    auto gray_l = [](int v) {
        float t[3];
        target(s_lift[255 - std::max(0, std::min(255, v))], 0, t);
        return t[0];
    };
    // the neutral entry nearest to g's target, then the gray whose target is nearest to that entry
    float    l    = gray_l(g);
    unsigned best = s_neutrals[0];
    for(int k = 1; k < s_n_neutral; k++)
        if(fabsf(s_lab[s_neutrals[k]][0] - l) < fabsf(s_lab[best][0] - l)) best = s_neutrals[k];
    int   out = g;
    float bd  = 1e9f;
    for(int v = 0; v < 256; v++)
    {
        float d = fabsf(gray_l(v) - s_lab[best][0]);
        if(d < bd) bd = d, out = v;
    }
    return out;
}

uint8_t* tones_frame()
{
    return s_frame;
}

// ---------------------------------------------------------------- dither

void tone_dither::alloc(int w_)
{
    w = w_;
    for(auto& r : rows) r.assign(static_cast<size_t>(w + 4) * 3, 0.0f);
}

void tone_dither::release()
{
    for(auto& r : rows) std::vector<float>().swap(r);
}

void tone_dither::row(const uint8_t* c, int y, uint8_t* out)
{
    // rp[k] receives the error for k rows down. Taps past either end of the row land in the 2 padding
    // cells, which are never read.
    float*        rp[3] = {rows[0].data(), rows[1].data(), rows[2].data()};
    float*        cur   = rp[0];
    const int8_t* noise = s_noise[y & (NOISE_N - 1)];
    int           d     = (y & 1) ? -1 : 1;
    for(int i = 0; i < w; i++)
    {
        int x  = d > 0 ? i : w - 1 - i;
        int r  = c[3 * x], g = c[3 * x + 1], b = c[3 * x + 2];
        int mx  = std::max(r, std::max(g, b));
        int mn  = std::min(r, std::min(g, b));
        int ink = 255 - mx;
        int red = std::max(0, r - std::max(g, b));
        // A colour that is neither neutral nor red-dominant (blue, green, purple) has only its brightest
        // channel in the ink rule and would land near paper. Its ink is raised until the target lightness
        // equals the pixel's own Oklab lightness, so blue text reads as a dark gray. Grays and reds keep
        // the judged mapping.
        if(red == 0 && mx - mn > CHROMA_MIN)
        {
            int by_l = static_cast<int>(255.0f * (s_l_max - pixel_L(r, g, b)) / (s_l_max - s_l_min) + 0.5f);
            ink      = std::max(ink, std::min(255, by_l));
        }
        ink = s_lift[ink];
        if(ink + red > 255) red = 255 - ink;
        bool   gray = red < TONEMAP_GRAY_MAX_RED;
        float* e    = cur + (x + 2) * 3;
        // paper with no carried error: the common case on a text page
        if(mx == 255 && red == 0 && fabsf(e[0]) < ERR_ZERO && fabsf(e[1]) < ERR_ZERO && fabsf(e[2]) < ERR_ZERO)
        {
            out[x] = TONE_PAPER;
            continue;
        }
        float t[3];
        target(ink, red, t);

        float el = std::max(-ERR_CLAMP_L, std::min(ERR_CLAMP_L, e[0]));
        el       = std::max(s_l_min - t[0], std::min(s_l_max - t[0], el));
        float v[3] = {t[0] + el, t[1], t[2]};
        // A gray pixel carries only lightness error: neutral entries cannot pay chroma back, so a
        // carried tint would only bias which gray is picked and ride through to the next colour.
        if(!gray)
        {
            v[1] += std::max(-ERR_CLAMP_C, std::min(ERR_CLAMP_C, e[1]));
            v[2] += std::max(-ERR_CLAMP_C, std::min(ERR_CLAMP_C, e[2]));
        }

        // A pixel whose target is paper or black takes no noise. Its dots come from carried error
        // alone, so text and the paper around it stay clean.
        const int nz = (ink == 0 && red == 0) || ink == 255 ? 0 : noise[x & (NOISE_N - 1)];
        unsigned  best;
        if(gray)
        {
            // A gray mixes only the two neutral entries that bracket its target, and the carried error
            // is held inside that pair. A pixel on an entry stays on it, and no error from an edge can
            // turn into a dot of a tone two steps away.
            int k = 0;
            while(k < s_n_neutral - 1 && s_lab[s_neutrals[k]][0] > t[0]) k++;
            unsigned lo = s_neutrals[k], hi = s_neutrals[k > 0 ? k - 1 : 0];
            // below the darkest entry, or exactly on an entry (black text, a solid fill): no mixing
            if(s_lab[lo][0] > t[0] || t[0] - s_lab[lo][0] < 1e-4f) hi = lo;
            v[0] = std::max(s_lab[lo][0], std::min(s_lab[hi][0], v[0]));
            if(lo == hi) best = lo;
            else
            {
                float dlo = v[0] - s_lab[lo][0], dhi = s_lab[hi][0] - v[0];
                float gap = s_lab[hi][0] - s_lab[lo][0];
                best      = dlo - dhi + NOISE_REL * nz * gap > 0 ? hi : lo;
            }
        } else
        {
            const uint8_t* cand = s_nb[s_lut[ink >> 2][red >> 2]];
            unsigned       next = cand[0];
            float          bd = 1e9f, nd = 1e9f;
            float          q[3] = {v[0] + NOISE_STEP * nz, v[1], v[2]};
            best                = cand[0];
            for(int k = 0; k < NB; k++)
            {
                unsigned f  = cand[k];
                float    dd = dist(q, s_lab[f]);
                if(dd < bd) nd = bd, next = best, bd = dd, best = f;
                else if(dd < nd) nd = dd, next = f;
            }
            if(NOISE_REL > 0 && nz && next != best)
            {
                // squared distances to the darker (lo) and lighter (hi) of the two nearest: the search
                // point lies past their midpoint by (d_lo - d_hi) / (2 |hi - lo|^2) of the gap
                unsigned lo = best, hi = next;
                float    dlo = bd, dhi = nd;
                if(s_lab[lo][0] > s_lab[hi][0]) std::swap(lo, hi), std::swap(dlo, dhi);
                best = dlo - dhi + NOISE_REL * nz * dist(s_lab[lo], s_lab[hi]) > 0 ? hi : lo;
            }
        }
        out[x] = static_cast<uint8_t>(best);

        const float kc     = TONES_ERR_KEEP_C;
        float       res[3] = {v[0] - s_lab[best][0], gray ? 0.0f : (v[1] - s_lab[best][1]) * kc,
                              gray ? 0.0f : (v[2] - s_lab[best][2]) * kc};
        const dither_taps& kt =
            HYBRID_NEAR2 > 0 && dist(v, s_lab[best]) < HYBRID_NEAR2 ? k_kernels[KERNEL_ATKINSON] : KERNEL;
        for(int j = 0; j < kt.n; j++)
        {
            const dither_tap& tp = kt.t[j];
            float*            p  = rp[tp.dy] + (x + tp.dx * d + 2) * 3;
            p[0] += res[0] * tp.w, p[1] += res[1] * tp.w, p[2] += res[2] * tp.w;
        }
    }
    std::swap(rows[0], rows[1]);
    std::swap(rows[1], rows[2]);
    std::fill(rows[2].begin(), rows[2].end(), 0.0f);
}

// ---------------------------------------------------------------- draw

bool tones_draw(const uint8_t* frame, tone_draw_stats* st)
{
    tone_draw_stats local;
    if(!st) st = &local;
    *st        = tone_draw_stats();
    int64_t t0 = esp_timer_get_time();

    uint32_t count[TONEMAP_COUNT] = {0};
    for(size_t p = 0; p < static_cast<size_t>(TONE_W) * TONE_H; p++) count[frame[p]]++;
    unsigned steps = 0;
    for(unsigned e = 0; e < N; e++)
    {
        if(!count[e]) continue;
        st->entries++;
        steps = std::max(steps, static_cast<unsigned>(strlen(k_tonemap[e].pattern)));
    }

    if(!s_seq_ok)
    {
        bool ok      = draw_stock(frame);
        st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        return ok;
    }

    epd_erase_stats_t es;
    esp_err_t         err = epd_verb_erase(TONES_ERASE, TONES_UNIT, &es);
    if(err == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "panel has no seq tables, drawing the three stock colours");
        s_seq_ok     = false;
        bool ok      = draw_stock(frame);
        st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        return ok;
    }
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        return false;
    }
    st->erase_ms = es.pull_ms;
    if(steps == 0)
    {
        // an all-paper frame: the erase is the whole draw
        epd_clear();
        st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        return true;
    }

    int64_t t1 = esp_timer_get_time();
    err        = epd_verb_open_table(TONES_UNIT, 0, EPD_VERB_TABLE_SEQ);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "seq open failed: %s", esp_err_to_name(err));
        st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        return false;
    }
    st->open_ms = static_cast<uint32_t>((esp_timer_get_time() - t1) / 1000);

    // Every pixel plays character k of its entry's pattern, hold past its end. The COG swaps its two
    // plane registers after every pull, so a pull without a send repeats the planes from two pulls
    // back. The send is skipped only when every used entry has the same table at k and k - 1 and that
    // table is hold or red.
    uint8_t tab[TONEMAP_COUNT], prev[TONEMAP_COUNT];
    bool    ok = true;
    for(unsigned k = 0; k < steps; k++)
    {
        bool repeat = k > 0;
        for(unsigned e = 0; e < N; e++)
        {
            tab[e] = static_cast<uint8_t>(table_at(e, k));
            if(repeat && count[e] && !(tab[e] == prev[e] && (tab[e] == 0 || tab[e] == 3))) repeat = false;
        }
        uint32_t ms = 0;
        if(repeat) err = epd_verb_pull(1, false, &ms);
        else
        {
            build_planes(frame, tab);
            err = epd_verb_pass(1, &ms);
            st->sends++;
        }
        st->pull_ms += ms;
        st->steps = k + 1;
        memcpy(prev, tab, sizeof(prev));
        if(err != ESP_OK)
        {
            ESP_LOGE(TAG, "step %u of %u failed: %s", k + 1, steps, esp_err_to_name(err));
            ok = false;
            break;
        }
    }
    epd_verb_close();
    epd_clear();
    st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "draw: %u entries, %u steps, %u sends, erase %u ms, open %u ms, pulls %u ms, total %u ms",
             (unsigned)st->entries, (unsigned)st->steps, (unsigned)st->sends, (unsigned)st->erase_ms,
             (unsigned)st->open_ms, (unsigned)st->pull_ms, (unsigned)st->total_ms);
    return ok;
}
