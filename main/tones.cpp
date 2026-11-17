// Tone rendering. See tones.h.

#include "tones.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "epd97.h"
#include "tone_plan.h"
#include "tonemap.h"
#include "tonemap_squeeze.h"

static const char* TAG = "tones";

// Carried error limit per channel in linear light, as a share of the paper's light. Black is 0 and
// paper 1. The widest gap between neighbouring neutral entries is 0.215 (L 0.9647 to L 0.8805), so
// the limit passes it.
static const float ERR_MAX = 0.25f;
// A carried error below this in every channel counts as none: a paper pixel then costs no search
// or diffusion.
static const float ERR_ZERO = 1e-4f;
// A target whose a, b chroma after the squeeze is below this is neutral. Float rounding leaves a
// gray's chroma near 1e-7, and the squeeze sets a blue or a green to exactly 0. The palest coloured
// entry has chroma 0.0078.
static const float NEUTRAL_C = 1e-3f;
// A neutral target within this Oklab L of a neutral entry is on that entry. The table gives L to 4
// decimals, and in the midtones one gray byte is 0.003 to 0.006 in L from the next. At 0.002 a gray
// byte lands on 20 of the 22 neutral entries. It misses L 0.676, which lies 0.002 below the entry at
// L 0.678, and L 0.058, between bytes 0 and 1.
static const float ON_ENTRY_L = 0.002f;
// Candidates searched per coloured pixel: the nearest entry to its target without error, and that
// entry's nearest neighbours in the weighted Oklab distance, itself first. The carried error moves
// the search point far from the target where a red lies outside the entries' linear light hull. On
// the host harness, against a search of all entries, 24 candidates change 6 to 13% of the pixels of
// the red ramp and the colour wedges and leave seams in the wedges, 48 change 0.3% at most.
static const int NB = 48;
// Targets kept by RGB, 2^CACHE_BITS slots. A page has few distinct colours. In a photo the miss
// costs one Oklab conversion, the squeeze and a nearest entry search pruned by L.
static const int CACHE_BITS = 10;
// Luminance of linear sRGB. A neutral pixel takes this share of each channel of its carried error.
static const float LUMA[3] = {0.2126f, 0.7152f, 0.0722f};
// Stock draw, for a panel without the tables: an entry is red when its a, b chroma is above
// STOCK_RED_C (half the red reference's 0.18), else black when its L is below STOCK_BLACK_L, else
// white.
static const float STOCK_RED_C   = 0.09f;
static const float STOCK_BLACK_L = 0.5f;

// Dither settings. The host harness builds each combination with -D overrides and compares them
// (host_tests/tones/README.md).
// Kernel: KERNEL_ATKINSON (6 taps, diffuses 6/8 of the error), KERNEL_FLOYD_STEINBERG (4 taps),
// KERNEL_JJN or KERNEL_STUCKI (12 taps). All fit the three error rows.
// Atkinson is the page default: it keeps text and graphics edges solid. The full-error kernels are
// for photo regions, where its dropped quarter of the error shows as steps in dark gradients.
#ifndef TONES_DITHER_KERNEL
#define TONES_DITHER_KERNEL KERNEL_ATKINSON
#endif
// Threshold noise amplitude in Oklab L, coloured pixels only: the search point's L moves by up to
// this much from a 64x64 table. The carried error is formed without it, so the noise moves dots
// and not the mean tone. 0 turns it off.
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
// Share of a coloured pixel's residual off its luminance that is diffused. The residual's
// luminance is always diffused in full. 1 diffuses the whole residual.
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
const unsigned     CACHE_SLOTS  = 1u << CACHE_BITS;

static_assert(static_cast<size_t>(TONE_W) * TONE_H == static_cast<size_t>(EPD_PLANE_BYTES) * 8, "frame and planes differ");
static_assert(TONE_W == EPD_W && TONE_H == EPD_H, "frame and panel differ");

// The target of one pixel colour, as the dither uses it. lin is the squeezed colour in linear light.
// A neutral target has lin equal on all three channels and holds in lo and hi the two neutral
// entries around it, lo the darker, the same entry when it is on one. A coloured target holds its
// nearest entry in lo. key is the RGB with bit 24 set, 0 for an empty slot.
struct target
{
    uint32_t key;
    float    lin[3];
    uint8_t  lo, hi;
    bool     neutral;
};

float    s_lab[TONEMAP_COUNT][3]; // entry Oklab
float    s_lin[TONEMAP_COUNT][3]; // entry linear light
uint8_t  s_nb[TONEMAP_COUNT][NB];
uint8_t  s_by_l[TONEMAP_COUNT];      // all entries, darkest first, for the nearest entry search
uint8_t  s_neutrals[TONEMAP_COUNT];  // the neutral entries, lightest first
float    s_neutral_l[TONEMAP_COUNT]; // and their L
int      s_n_neutral = 0;
float    s_byte_lin[256]; // sRGB byte to linear
uint8_t  s_black = 0;
uint8_t* s_frame = nullptr;
bool     s_seq_ok = true; // false once the driver reported no tables
int8_t   s_noise[NOISE_N][NOISE_N]; // threshold noise, -128 to 127, uniform
target*  s_cache = nullptr; // CACHE_SLOTS targets in PSRAM, from tones_init()
#ifdef TONES_BENCH
int      s_bench_bits = CACHE_BITS; // cache index bits for tones_bench(), 0 for no cache
uint32_t s_bench_miss = 0, s_bench_look = 0;
#endif

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

// Linear sRGB to Oklab and back.
inline void lin_to_oklab(const float c[3], float out[3])
{
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void oklab_to_lin(const float lab[3], float out[3])
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

// sRGB bytes to Oklab, the form tonemap_squeeze_begin() takes.
void oklab(uint8_t r8, uint8_t g8, uint8_t b8, float out[3])
{
    const float c[3] = {s_byte_lin[r8], s_byte_lin[g8], s_byte_lin[b8]};
    lin_to_oklab(c, out);
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

inline float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

// The nearest entry to t over all entries. The scan starts at t's L in s_by_l and widens to the
// side nearer in L until the L gap alone is past the best distance.
unsigned nearest(const float t[3])
{
    const int n  = static_cast<int>(N);
    int       up = static_cast<int>(
        std::lower_bound(s_by_l, s_by_l + n, t[0], [](uint8_t e, float l) { return s_lab[e][0] < l; }) - s_by_l);
    int      dn   = up - 1;
    unsigned best = s_by_l[std::min(up, n - 1)];
    float    bd   = 1e36f;
    for(;;)
    {
        const float gu = up < n ? s_lab[s_by_l[up]][0] - t[0] : 1e18f;
        const float gd = dn >= 0 ? t[0] - s_lab[s_by_l[dn]][0] : 1e18f;
        const float gap = std::min(gu, gd);
        if(gap * gap >= bd) break;
        const unsigned e = gu <= gd ? s_by_l[up++] : s_by_l[dn--];
        const float    d = dist(t, s_lab[e]);
        if(d < bd) bd = d, best = e;
    }
    return best;
}

// The two neutral entries around L, lo the darker. Both are the same entry when L is within
// ON_ENTRY_L of it, above the lightest or below the darkest.
void bracket(float L, uint8_t* lo, uint8_t* hi)
{
    int k = 0;
    while(k < s_n_neutral - 1 && s_neutral_l[k] > L + ON_ENTRY_L) k++;
    *lo = s_neutrals[k];
    *hi = k == 0 || L - s_neutral_l[k] <= ON_ENTRY_L ? *lo : s_neutrals[k - 1];
}

// The target of an sRGB colour: its Oklab through tonemap_squeeze(), L held at or above the black
// entry's.
void make_target(uint8_t r, uint8_t g, uint8_t b, target* t)
{
    float lab[3];
    oklab(r, g, b, lab);
    tonemap_squeeze(lab);
    lab[0]     = std::max(lab[0], s_lab[s_black][0]);
    t->neutral = lab[1] * lab[1] + lab[2] * lab[2] < NEUTRAL_C * NEUTRAL_C;
    if(t->neutral)
    {
        bracket(lab[0], &t->lo, &t->hi);
        t->lin[0] = t->lin[1] = t->lin[2] = lab[0] * lab[0] * lab[0];
    }
    else
    {
        oklab_to_lin(lab, t->lin);
        t->lo = t->hi = static_cast<uint8_t>(nearest(lab));
    }
}

inline const target& target_at(uint8_t r, uint8_t g, uint8_t b)
{
    const uint32_t key = 0x1000000u | static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | b;
#ifdef TONES_BENCH
    s_bench_look++;
    if(s_bench_bits == 0)
    {
        static target tmp;
        make_target(r, g, b, &tmp);
        s_bench_miss++;
        return tmp;
    }
    target& t = s_cache[(key * 0x9e3779b1u) >> (32 - s_bench_bits)];
    if(t.key != key) s_bench_miss++;
#else
    target&        t   = s_cache[(key * 0x9e3779b1u) >> (32 - CACHE_BITS)];
#endif
    if(t.key != key)
    {
        make_target(r, g, b, &t);
        t.key = key;
    }
    return t;
}

void build_planes(const uint8_t* frame, const uint8_t code[TONEMAP_COUNT])
{
    uint8_t* bw  = epd_plane_bw();
    uint8_t* red = epd_plane_red();
    for(size_t i = 0; i < EPD_PLANE_BYTES; i++)
    {
        const uint8_t* px = frame + i * 8;
        uint8_t        b = 0, r = 0;
        for(unsigned bit = 0; bit < 8; bit++)
        {
            const uint8_t t = code[px[bit]];
            if(t & 1) b |= static_cast<uint8_t>(0x80u >> bit);
            if(t & 2) r |= static_cast<uint8_t>(0x80u >> bit);
        }
        bw[i]  = b;
        red[i] = r;
    }
}

// The three stock colours, for a panel without the tables. Stock red is the red bit alone (plane
// pair 2), stock black the bw bit alone (pair 1).
bool draw_stock(const uint8_t* frame)
{
    uint8_t code[TONEMAP_COUNT];
    for(unsigned e = 0; e < N; e++)
        code[e] = hypotf(s_lab[e][1], s_lab[e][2]) > STOCK_RED_C ? 2 : s_lab[e][0] < STOCK_BLACK_L ? 1 : 0;
    build_planes(frame, code);
    return refresh_display(EPD_SLOW);
}

// A session on the phase's table, reopened when the open one differs. Phase 1 opens
// EPD_VERB_TABLE_PHASE1 at unit 1 (the driver sets its drive), phase 2 EPD_VERB_TABLE_SEQ at
// TONE_NUDGE_FRAMES.
esp_err_t session_open(tone_phase ph)
{
    const epd_verb_table_t table = ph == TONE_PHASE_1 ? EPD_VERB_TABLE_PHASE1 : EPD_VERB_TABLE_SEQ;
    const uint8_t          unit  = ph == TONE_PHASE_1 ? 1 : TONE_NUDGE_FRAMES;
    if(epd_verb_is_open() && epd_verb_table() == table && (table != EPD_VERB_TABLE_SEQ || epd_verb_unit() == unit))
        return ESP_OK;
    return epd_verb_open_table(unit, 0, table);
}

// Every pull of phase 1, then every pull of phase 2. At pull k each counted entry plays
// tone_plan_code(), the others code 0. The COG swaps its two plane registers after every pull, so
// a pull without a plane send plays the planes of two pulls back. A pull skips the send only when
// it is not the first of its phase and every counted entry's code is 0 or 3 and equals its code at
// the pull before. ESP_ERR_NOT_SUPPORTED only when the first session open reports it, with nothing
// played. The caller closes the session.
esp_err_t play(const uint8_t* frame, const uint32_t count[TONEMAP_COUNT], tone_draw_stats* st)
{
    tone_plan plan;
    if(!tone_plan_build(count, &plan))
    {
        ESP_LOGE(TAG, "no plan: the tone map is not expanded");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t code[TONEMAP_COUNT], prev[TONEMAP_COUNT];
    memset(prev, 0, sizeof(prev));
    bool opened = false;
    for(int p = 0; p < TONE_PHASE_COUNT; p++)
    {
        const tone_phase ph  = static_cast<tone_phase>(p);
        const unsigned   len = plan.len[ph];
        if(len == 0) continue;
        const int64_t t1  = esp_timer_get_time();
        esp_err_t     err = session_open(ph);
        st->open_ms += static_cast<uint32_t>((esp_timer_get_time() - t1) / 1000);
        if(err != ESP_OK)
        {
            if(err == ESP_ERR_NOT_SUPPORTED && !opened) return err;
            ESP_LOGE(TAG, "phase %d open failed: %s", p + 1, esp_err_to_name(err));
            return err == ESP_ERR_NOT_SUPPORTED ? ESP_FAIL : err;
        }
        opened = true;
        for(unsigned k = 0; k < len; k++)
        {
            bool repeat = k > 0;
            for(unsigned e = 0; e < N; e++)
            {
                if(!count[e])
                {
                    code[e] = 0;
                    continue;
                }
                code[e] = tone_plan_code(plan, e, ph, k);
                if(code[e] != prev[e] || (code[e] != 0 && code[e] != 3)) repeat = false;
            }
            uint32_t ms = 0;
            if(repeat) err = epd_verb_pull(1, false, &ms);
            else
            {
                build_planes(frame, code);
                err = epd_verb_pass(1, &ms);
                st->sends++;
            }
            st->pull_ms += ms;
            memcpy(prev, code, sizeof(prev));
            if(err != ESP_OK)
            {
                ESP_LOGE(TAG, "phase %d pull %u of %u failed: %s", p + 1, k + 1, len, esp_err_to_name(err));
                return err;
            }
            (ph == TONE_PHASE_1 ? st->pulls1 : st->pulls2)++;
        }
    }
    return ESP_OK;
}
} // namespace

bool tones_init()
{
    char why[192];
    if(!tone_plan_init(why, sizeof(why)))
    {
        ESP_LOGE(TAG, "tone map refused: %s", why);
        return false;
    }
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        s_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    const int eb = entry_of(TONEMAP_BLACK_PATTERN);
    if(eb < 0 || strcmp(k_tonemap[TONE_PAPER].pattern, TONEMAP_PAPER_PATTERN) != 0)
    {
        ESP_LOGE(TAG, "tone map lacks the black entry, or paper is not entry %d", TONE_PAPER);
        return false;
    }
    s_black     = static_cast<uint8_t>(eb);
    s_n_neutral = 0;
    for(unsigned e = 0; e < N; e++)
    {
        const tone_entry& t = k_tonemap[e];
        s_lab[e][0] = t.L, s_lab[e][1] = t.a, s_lab[e][2] = t.b;
        oklab_to_lin(s_lab[e], s_lin[e]);
        s_by_l[e] = static_cast<uint8_t>(e);
        // Any r or R pull makes a pink, so only a pattern without them is neutral. The table gives
        // such an entry a and b of 0, and the neutral path takes its light as L cubed.
        if(strpbrk(t.pattern, "rR")) continue;
        if(t.a != 0.0f || t.b != 0.0f)
        {
            ESP_LOGE(TAG, "neutral entry %u (%s) has a %.4f b %.4f, not 0", e, t.pattern, static_cast<double>(t.a),
                     static_cast<double>(t.b));
            return false;
        }
        s_lin[e][0] = s_lin[e][1] = s_lin[e][2] = t.L * t.L * t.L;
        s_neutrals[s_n_neutral++] = static_cast<uint8_t>(e);
    }
    std::sort(s_neutrals, s_neutrals + s_n_neutral, [](uint8_t a, uint8_t b) { return s_lab[a][0] > s_lab[b][0]; });
    for(int k = 0; k < s_n_neutral; k++) s_neutral_l[k] = s_lab[s_neutrals[k]][0];
    std::sort(s_by_l, s_by_l + N, [](uint8_t a, uint8_t b) { return s_lab[a][0] < s_lab[b][0]; });

    for(unsigned e = 0; e < N; e++)
    {
        // the NB nearest entries to e, e itself first
        std::vector<std::pair<float, unsigned>> d;
        d.reserve(N);
        for(unsigned f = 0; f < N; f++) d.push_back({dist(s_lab[e], s_lab[f]), f});
        std::sort(d.begin(), d.end());
        for(int k = 0; k < NB; k++) s_nb[e][k] = static_cast<uint8_t>(d[k].second);
    }
    tonemap_squeeze_begin(oklab);
    build_noise();

    // Both in PSRAM: the browser task needs its stack as one block of internal RAM.
    if(!s_cache) s_cache = static_cast<target*>(heap_caps_malloc(CACHE_SLOTS * sizeof(target), MALLOC_CAP_SPIRAM));
    if(!s_frame)
        s_frame = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(TONE_W) * TONE_H, MALLOC_CAP_SPIRAM));
    if(!s_cache || !s_frame)
    {
        ESP_LOGE(TAG, "no PSRAM for the frame or the target cache");
        return false;
    }
    memset(s_cache, 0, CACHE_SLOTS * sizeof(target));
    memset(s_frame, TONE_PAPER, static_cast<size_t>(TONE_W) * TONE_H);
    ESP_LOGI(TAG, "%u entries, %d neutral, black entry %u", N, s_n_neutral, (unsigned)s_black);
    return true;
}

uint8_t tones_black()
{
    return s_black;
}

int tones_solid_gray(int g)
{
    g = std::max(0, std::min(255, g));
    target tg;
    make_target(static_cast<uint8_t>(g), static_cast<uint8_t>(g), static_cast<uint8_t>(g), &tg);
    const float l = cbrtf(tg.lin[0]);
    // Of the grays whose target is on a neutral entry, the one whose entry is nearest to g's target
    // in L, then nearest to g.
    int   out = g;
    float bd  = 1e9f;
    for(int v = 0; v < 256; v++)
    {
        target t;
        make_target(static_cast<uint8_t>(v), static_cast<uint8_t>(v), static_cast<uint8_t>(v), &t);
        if(!t.neutral || t.lo != t.hi) continue;
        const float d = fabsf(s_lab[t.lo][0] - l) + 1e-6f * static_cast<float>(abs(v - g));
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
        const int     x = d > 0 ? i : w - 1 - i;
        const uint8_t r = c[3 * x], g = c[3 * x + 1], b = c[3 * x + 2];
        const float*  e = cur + (x + 2) * 3;
        float         ec[3];
        bool          none = true;
        for(int k = 0; k < 3; k++)
        {
            ec[k] = clampf(e[k], -ERR_MAX, ERR_MAX);
            none  = none && fabsf(ec[k]) < ERR_ZERO;
        }
        // paper with no carried error: the common case on a text page
        if(none && (r & g & b) == 255)
        {
            out[x] = TONE_PAPER;
            continue;
        }
        const target& t  = target_at(r, g, b);
        const int     nz = noise[x & (NOISE_N - 1)];
        unsigned      best;
        float         res[3];
        float         q[3] = {0.0f, 0.0f, 0.0f}; // the search point in Oklab
        if(t.neutral)
        {
            // A neutral pixel mixes only the two neutral entries around its target, and the carried
            // error is held inside that pair. A pixel on an entry takes it and leaves no error, so
            // black text and a solid fill stay solid and no error from an edge can turn into a dot of
            // a tone two steps away. Neutral entries cannot pay a tint back, so the pixel carries the
            // luminance of the error alone.
            if(t.lo == t.hi)
            {
                out[x] = t.lo;
                continue;
            }
            const float lo = s_lin[t.lo][0], hi = s_lin[t.hi][0];
            const float v  = clampf(t.lin[0] + LUMA[0] * ec[0] + LUMA[1] * ec[1] + LUMA[2] * ec[2], lo, hi);
            // The decision point: the pair's midpoint in Oklab L, moved by the noise, compared in
            // linear light as its cube.
            const float llo = s_lab[t.lo][0], lhi = s_lab[t.hi][0];
            const float m   = 0.5f * (llo + lhi - NOISE_REL * nz * (lhi - llo));
            best            = v > m * m * m ? t.hi : t.lo;
            res[0] = res[1] = res[2] = v - s_lin[best][0];
            if(HYBRID_NEAR2 > 0) q[0] = cbrtf(v), q[1] = q[2] = 0.0f;
        }
        else
        {
            float v[3];
            for(int k = 0; k < 3; k++) v[k] = clampf(t.lin[k] + ec[k], 0.0f, 1.0f);
            lin_to_oklab(v, q);
            const uint8_t* cand = s_nb[t.lo];
            unsigned       next = cand[0];
            float          bd = 1e9f, nd = 1e9f;
            const float    p[3] = {q[0] + NOISE_STEP * nz, q[1], q[2]};
            best                = cand[0];
            for(int k = 0; k < NB; k++)
            {
                unsigned f  = cand[k];
                float    dd = dist(p, s_lab[f]);
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
            for(int k = 0; k < 3; k++) res[k] = v[k] - s_lin[best][k];
            if(TONES_ERR_KEEP_C != 1.0f)
            {
                const float yl = LUMA[0] * res[0] + LUMA[1] * res[1] + LUMA[2] * res[2];
                for(float& rk : res) rk = yl + TONES_ERR_KEEP_C * (rk - yl);
            }
        }
        out[x] = static_cast<uint8_t>(best);

        const dither_taps& kt =
            HYBRID_NEAR2 > 0 && dist(q, s_lab[best]) < HYBRID_NEAR2 ? k_kernels[KERNEL_ATKINSON] : KERNEL;
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
    *st              = tone_draw_stats();
    const int64_t t0 = esp_timer_get_time();

    uint32_t count[TONEMAP_COUNT] = {0};
    for(size_t p = 0; p < static_cast<size_t>(TONE_W) * TONE_H; p++) count[frame[p]]++;
    for(unsigned e = 0; e < N; e++) st->entries += count[e] != 0;

    bool ok = false;
    if(s_seq_ok)
    {
        const esp_err_t err = play(frame, count, st);
        epd_verb_close();
        epd_clear();
        if(err == ESP_ERR_NOT_SUPPORTED)
        {
            ESP_LOGW(TAG, "panel has no tables, drawing the three stock colours");
            s_seq_ok = false;
        }
        ok = err == ESP_OK;
    }
    if(!s_seq_ok) ok = draw_stock(frame);
    st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "draw %s: %u entries, %u + %u pulls, %u sends, open %u ms, pulls %u ms, total %u ms",
             ok ? "ok" : "failed", (unsigned)st->entries, (unsigned)st->pulls1, (unsigned)st->pulls2,
             (unsigned)st->sends, (unsigned)st->open_ms, (unsigned)st->pull_ms, (unsigned)st->total_ms);
    return ok;
}

#ifdef TONES_BENCH
#include "tones_bench.inc"
#endif
