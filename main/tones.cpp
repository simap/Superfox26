// Tone rendering. See tones.h.
//
// The dither runs in fixed point on the tone_fx kernels (tone_fx.h). A pixel needs the error the
// pixel before it left, and nothing else in its target does. So a row runs in chunks of CHUNK
// pixels, each in two passes.
//
// Pass A, per chunk, no dependence between pixels. all_white() finds a chunk of white pixels on its
// sRGB bytes, and such a chunk is paper with no decode. Otherwise decode() decodes the chunk's sRGB
// bytes straight from the caller's row and compact() gives each pixel a slot. Every white pixel
// takes the slot WHITE, whose target tones_init() places. A pixel with the linear light of the
// last pixel kept takes that pixel's slot, so a text row of glyphs on paper needs a few slots per
// chunk. The other kernels then run once over the slots, padded to 8: linear light to Oklab, the
// squeeze, Oklab to linear light when a slot is coloured and the neutral bracket when a slot is
// neutral. Each slot ends as one of slot_kind with its target in linear light.
//
// Pass B, pixel by pixel in scan order: the carried error, the decision and the diffusion. Pass A
// runs left to right on every row and pass B walks the chunk in scan order, so no array holds more
// than one chunk. The chunks of a right to left row run from the right end. With the Atkinson
// kernel (FAST), run_b() takes the pixels of K_ENTRY and K_NEUTRAL slots in a loop that calls
// nothing, and run_c() the coloured ones, which call the kernels.
//
// CHUNK is 160, 6 chunks per 960 pixel row. The kernels after the decode run once per chunk on the
// slots rounded up to 8. A larger chunk wastes fewer padding lanes on text rows and pays the fixed
// cost of a kernel call fewer times. The arrays take 3.3 KB.
//
// The kernels are called by their plain names: the vector kernels of main/ on the device, their
// stand ins of the same bits on the host (tone_fx.h).
//
// The sRGB decode reads up to 16 bytes past the last pixel of a chunk, so the caller's RGB buffer
// holds at least 16 readable bytes after the last pixel of its last row.
//
// Tables. Everything the dither reads that depends only on the tone table and fixed constants is
// data from main/tone_tables.inc (tone_tables.h), built on the host. The tables that also depend on
// the dither settings below (the neutral pairs and the white noise) the compiler builds from the
// tone table. tones_init() computes none of them.

#include "tones.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "epd97.h"
#include "tone_fx.h"
#include "tone_fx_bracket.h"
#include "tone_fx_oklab.h"
#include "tone_fx_search.h"
#include "tone_fx_squeeze.h"
#include "tone_fx_srgb.h"
#include "tone_plan.h"
#include "tone_tables.h"
#include "tonemap.h"
#if TONES_HAVE_FLOAT_REF
#include "tonemap_squeeze.h"
#endif
#ifdef TONES_BENCH
#include "esp_cpu.h"
#endif

static const char* TAG = "tones";

// Carried error limit per channel in linear light, 0.25 of the paper's light in q15. Black is 0 and
// paper 1. The widest gap between neighbouring neutral entries is 0.215 (L 0.9647 to L 0.8805), so
// the limit passes it.
static const int ERR_MAX_Q = 8192;
// A carried error of at most this many q15 counts in every channel counts as none (below 1e-4): a
// paper pixel then costs no search or diffusion.
static const int ERR_ZERO_Q = 3;
// A target whose chroma along the hue of sRGB red after the squeeze is below this many q15 counts
// (0.001) is neutral. The squeeze sets a gray, a blue or a green to exactly 0, and the kernels leave
// a gray within a few counts of 0. The palest coloured entry has chroma 0.0078, 256 counts.
static const int NEUTRAL_C_Q = 33;
// Luminance of linear sRGB in q15, 0.2126, 0.7152 and 0.0722, summing to 32768. A neutral pixel
// takes this share of each channel of its carried error.
static const int LUMA_Q[3] = {6966, 23436, 2366};
// Stock draw, for a panel without the tables: an entry is red when its a, b chroma is above
// STOCK_RED_C (half the red reference's 0.18), else black when its L is below STOCK_BLACK_L, else
// white.
static const float STOCK_RED_C   = 0.09f;
static const float STOCK_BLACK_L = 0.5f;

// Dither settings. The host harness builds each combination with -D overrides and compares them
// (host_tests/tones/README.md). Each is a real number here and a whole number in the dither.
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
// 1 runs Atkinson through the tap loop of the other kernels. Both give the same bits. The host
// harness builds it to check the Atkinson path.
#ifndef TONES_DITHER_GENERIC
#define TONES_DITHER_GENERIC 0
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

// A tap of weight n / den. dx along the scan direction, dy rows down.
struct tap_q
{
    int8_t dx, dy, n;
};

struct taps_q
{
    int   n, den;
    tap_q t[12];
};

// Indexed by dither_kernel. Every tap stays within 2 columns and 2 rows, the reach of the error
// rows in tone_dither.
const taps_q k_taps[] = {
    {6, 8, {{1, 0, 1}, {2, 0, 1}, {-1, 1, 1}, {0, 1, 1}, {1, 1, 1}, {0, 2, 1}}},
    {4, 16, {{1, 0, 7}, {-1, 1, 3}, {0, 1, 5}, {1, 1, 1}}},
    {12, 48, {{1, 0, 7}, {2, 0, 5}, {-2, 1, 3}, {-1, 1, 5}, {0, 1, 7}, {1, 1, 5}, {2, 1, 3}, {-2, 2, 1}, {-1, 2, 3},
              {0, 2, 5}, {1, 2, 3}, {2, 2, 1}}},
    {12, 42, {{1, 0, 8}, {2, 0, 4}, {-2, 1, 2}, {-1, 1, 4}, {0, 1, 8}, {1, 1, 4}, {2, 1, 2}, {-2, 2, 1}, {-1, 2, 2},
              {0, 2, 4}, {1, 2, 2}, {2, 2, 1}}},
};

const taps_q& TAPS = k_taps[TONES_DITHER_KERNEL];
// The settings as whole numbers, folded at compile time. REL_Q is the noise fraction in 1/4096,
// NOISE_L_Q the L amplitude in q15, KEEP_C_Q the kept chroma share in q15, HYBRID_Q2 the hybrid
// distance squared in q15 counts squared.
const int      REL_Q     = static_cast<int>(TONES_DITHER_NOISE_REL * 4096.0f + 0.5f);
const int      NOISE_L_Q = static_cast<int>(TONES_DITHER_NOISE_L * 32768.0f + 0.5f);
const int      KEEP_C_Q  = static_cast<int>(TONES_ERR_KEEP_C * 32768.0f + 0.5f);
const double   HYBRID_D  = static_cast<double>(TONES_DITHER_HYBRID_NEAR) * 32768.0;
const uint32_t HYBRID_Q2 = static_cast<uint32_t>(HYBRID_D * HYBRID_D + 0.5);
const bool     HYBRID    = TONES_DITHER_HYBRID_NEAR > 0.0f;
// Atkinson with no hybrid runs its own path: the two taps along the row stay in registers, and the
// three error rows are written once per pixel each.
const bool     FAST      = TONES_DITHER_KERNEL == KERNEL_ATKINSON && !HYBRID && !TONES_DITHER_GENERIC;

static_assert(static_cast<size_t>(TONE_W) * TONE_H == static_cast<size_t>(EPD_PLANE_BYTES) * 8, "frame and planes differ");
static_assert(TONE_W == EPD_W && TONE_H == EPD_H, "frame and panel differ");
static_assert(TONEMAP_COUNT <= 128, "a slot of kind K_ENTRY holds the entry in a byte below NO_ENTRY");

// What pass A leaves in a slot.
enum slot_kind : uint8_t
{
    K_ENTRY,   // neutral and on the entry lo: the pixel takes it and leaves no error
    K_NEUTRAL, // neutral between the entries lo (darker) and hi: r holds the target, L cubed
    K_COLOUR,  // r, g and b hold the target
    K_WHITE,   // the slot WHITE when white is not on the paper entry: paper with no carried error,
               // else as s_white_kind
};

const int CHUNK = 160;
const int SLOTS = CHUNK + 16; // CHUNK slots, the slot WHITE, and arrays of a multiple of 16 bytes
const int WHITE = CHUNK;
static_assert(CHUNK % 8 == 0 && SLOTS % 16 == 0 && WHITE < 256, "chunk layout");
// chunk::ent of a slot that is not K_ENTRY. Pass B tests its top bit.
const uint8_t NO_ENTRY = 0xff;

// The working arrays of one chunk. Planar and 16 byte aligned for the kernels (tone_fx.h). The
// kernels write at most CHUNK slots, so the slot WHITE keeps what tones_init() put there.
struct chunk
{
    TONE_FX_ALIGNED q15 r[SLOTS]; // linear light: the decoded pixels, then the target per slot
    TONE_FX_ALIGNED q15 g[SLOTS];
    TONE_FX_ALIGNED q15 b[SLOTS];
    TONE_FX_ALIGNED q15 L[SLOTS]; // Oklab, then the squeezed colour
    TONE_FX_ALIGNED q15 A[SLOTS];
    TONE_FX_ALIGNED q15 B[SLOTS];
    TONE_FX_ALIGNED q15 C[SLOTS];     // the squeezed chroma along the hue of sRGB red
    TONE_FX_ALIGNED uint8_t lo[SLOTS]; // the neutral pair, or the entry of a K_ENTRY slot
    TONE_FX_ALIGNED uint8_t hi[SLOTS];
    TONE_FX_ALIGNED uint8_t kind[SLOTS];
    TONE_FX_ALIGNED uint8_t ent[SLOTS]; // lo of a K_ENTRY slot, else NO_ENTRY
    uint8_t                 slot[CHUNK]; // per pixel of the chunk, left to right
};

constexpr uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

constexpr bool same_str(const char* a, const char* b)
{
    for(; *a != '\0' && *a == *b; a++, b++) {}
    return *a == *b;
}

constexpr int entry_of(const char* pattern)
{
    for(unsigned e = 0; e < N; e++)
        if(same_str(k_tonemap[e].pattern, pattern)) return static_cast<int>(e);
    return -1;
}

static_assert(entry_of(TONEMAP_BLACK_PATTERN) >= 0, "the tone map has the black entry");
static_assert(same_str(k_tonemap[TONE_PAPER].pattern, TONEMAP_PAPER_PATTERN), "the paper is entry TONE_PAPER");

// A residual is at most Q15_ONE - lin_min or lin_max over the entries' linear light. Atkinson puts at
// most 6/8 of the largest residual into a cell, so the error rows hold it in 16 bits.
constexpr bool lin_fits_error_rows()
{
    int lin_min = 0, lin_max = 0;
    for(const auto& lin : tone_tables::qlin)
        for(int v : lin) lin_min = std::min(lin_min, v), lin_max = std::max(lin_max, v);
    return (std::max(Q15_ONE - lin_min, lin_max) * 6 + 7) / 8 + 8 <= 32767;
}
static_assert(lin_fits_error_rows(), "an entry's linear light runs past the error range");

// Per neutral pair, indexed by its lighter entry hi, in units of 2^-24: the midpoint of the two L,
// and the move of the decision point per unit of noise, TONES_DITHER_NOISE_REL / 128 of the gap.
// The neutral entries are those of the bracket kernel, lightest first.
struct pair_tables
{
    std::array<int32_t, TONEMAP_COUNT> mid, rel;
};

constexpr pair_tables make_pairs()
{
    pair_tables p{};
    for(int k = 1; k < tone_tables::neutral_count; k++)
    {
        const unsigned hi = tone_tables::neutral[k - 1], lo = tone_tables::neutral[k];
        const double   lh = k_tonemap[hi].L, ll = k_tonemap[lo].L;
        p.mid[hi]         = static_cast<int32_t>(0.5 * (ll + lh) * 16777216.0 + 0.5);
        p.rel[hi] =
            static_cast<int32_t>(static_cast<double>(TONES_DITHER_NOISE_REL) / 128.0 * (lh - ll) * 16777216.0 + 0.5);
    }
    return p;
}

// Threshold noise, -128 to 127, uniform. Blue: the table of main/tone_tables.inc. White: the hash
// of the cell index.
typedef std::array<std::array<int8_t, NOISE_N>, NOISE_N> noise_table;

constexpr noise_table white_noise()
{
    noise_table t{};
    for(int i = 0; i < NOISE_N * NOISE_N; i++)
    {
        const int h                 = static_cast<int>(hash32(static_cast<uint32_t>(i)) >> 24);
        t[i / NOISE_N][i % NOISE_N] = static_cast<int8_t>(h - 128);
    }
    return t;
}

// Entry Oklab, for the stock draw and the float dither. s_lab[e] is entry e's L, a, b.
struct lab_table
{
    float v[TONEMAP_COUNT][3];
    constexpr const float* operator[](unsigned e) const { return v[e]; }
};

constexpr lab_table make_lab()
{
    lab_table t{};
    for(unsigned e = 0; e < N; e++)
        t.v[e][0] = k_tonemap[e].L, t.v[e][1] = k_tonemap[e].a, t.v[e][2] = k_tonemap[e].b;
    return t;
}

TONE_FX_ALIGNED chunk s_ck;
// The kind of the slot WHITE when that is K_WHITE.
const uint8_t s_white_kind = tone_tables::white.white_kind;

// Read per pixel, so internal RAM, initialized data: per entry, q15, Oklab L, 2a and 2b (the chroma
// weight folded in, as in tone_fx_search.h), and the linear light, L cubed on all three channels
// for a neutral entry. Then the neutral pairs and the noise.
TONE_TABLES_RAM std::array<int16_t, TONEMAP_COUNT>                s_qL       = tone_tables::qL;
TONE_TABLES_RAM std::array<int16_t, TONEMAP_COUNT>                s_q2a      = tone_tables::q2a;
TONE_TABLES_RAM std::array<int16_t, TONEMAP_COUNT>                s_q2b      = tone_tables::q2b;
TONE_TABLES_RAM std::array<std::array<int16_t, 3>, TONEMAP_COUNT> s_qlin     = tone_tables::qlin;
TONE_TABLES_RAM std::array<int32_t, TONEMAP_COUNT>                s_pair_mid = make_pairs().mid;
TONE_TABLES_RAM std::array<int32_t, TONEMAP_COUNT>                s_pair_rel = make_pairs().rel;
TONE_TABLES_RAM noise_table s_noise = TONES_DITHER_NOISE_BLUE ? tone_tables::noise_blue : white_noise();

const lab_table s_lab   = make_lab(); // flash
const uint8_t   s_black = static_cast<uint8_t>(entry_of(TONEMAP_BLACK_PATTERN));
uint8_t*        s_frame  = nullptr;
bool            s_seq_ok = true; // false once the driver reported no tables

#ifdef TONES_BENCH
// The benchmark's split of the dither (tones_bench.inc). Cycles per phase, read at chunk boundaries
// in every bench run. The pixel counts per path come from a separate run of dither_row<true>,
// which tone_dither::row() takes while s_bench_count is set.
enum bench_phase
{
    PH_DECODE,  // the white test on the source bytes and the sRGB decode
    PH_COMPACT, // the slots of the pixels
    PH_KERNELS, // pass_a()
    PH_PASS_B,  // pass B, and the fill of an all white chunk
    PH_N
};
struct bench_counts
{
    uint32_t chunks, white_chunks, kernel_chunks, lanes, colour_chunks, neutral_chunks;
    uint32_t white_chunk_px; // pixels of the all white chunks
    uint32_t entry_run_px;   // K_ENTRY pixels with no carry, and of those the white ones
    uint32_t white_run_px;
    uint32_t entry_carry_px; // K_ENTRY pixels with a carry to pass on, and K_WHITE ones with no share
    uint32_t neutral_px, colour_px;
    uint32_t colour_two_px; // coloured pixels that took the noise decision between the two nearest
    uint32_t colour_runs;   // runs of pixels of kind K_COLOUR or K_WHITE, the calls of run_c()
};
uint32_t     s_bench_cyc[PH_N];
bench_counts s_bench_n;
bool         s_bench_count = false;
#define BENCH_NOW(v) const uint32_t v = esp_cpu_get_cycle_count()
#define BENCH_ADD(ph, t1, t0) s_bench_cyc[ph] += (t1) - (t0)
#define BENCH_COUNT(field, v)                    \
    do                                           \
    {                                            \
        if(COUNT) s_bench_n.field += (v);        \
    } while(0)
#else
#define BENCH_NOW(v)
#define BENCH_ADD(ph, t1, t0)
#define BENCH_COUNT(field, v) \
    do                        \
    {                         \
    } while(0)
#endif

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// L cubed, both q15, rounded to nearest. L is 0 or above.
inline int cube_q15(int l)
{
    return static_cast<int>((static_cast<int64_t>(l * l) * l + (1 << 29)) >> 30);
}

// The weighted Oklab distance of the point (l, a2, b2) to entry e, all q15 with a and b doubled, in
// q15 counts squared. A difference of up to 65535 squares exactly in 32 bits unsigned. The sum stays
// below 2^32 for every point the dither forms: L from -0.04 to 1, a and b of sRGB colours.
inline uint32_t dist_q(int l, int a2, int b2, unsigned e)
{
    const uint32_t dl = static_cast<uint32_t>(l - s_qL[e]);
    const uint32_t da = static_cast<uint32_t>(a2 - s_q2a[e]);
    const uint32_t db = static_cast<uint32_t>(b2 - s_q2b[e]);
    return dl * dl + da * da + db * db;
}

// res / 8 rounded to nearest, ties to even, so the shares carry no mean bias.
inline int share8(int res)
{
    return (res + 3 + ((res >> 3) & 1)) >> 3;
}

// x / den rounded to nearest, den above 0, for any sign of x.
inline int round_div(int64_t x, int den)
{
    const int64_t t = 2 * x + den, d = 2 * static_cast<int64_t>(den);
    return static_cast<int>(t >= 0 ? t / d : -((-t + d - 1) / d));
}

inline int16_t sat16(int v)
{
    return static_cast<int16_t>(clampi(v, -32768, 32767));
}

// The decode of n pixels, n not a multiple of 8, through a copy padded with white, so that it reads
// no byte past the 16 after the last pixel. Only a row width that is not a multiple of 8 needs it.
__attribute__((noinline)) void decode_short(const uint8_t* rgb, int n)
{
    uint8_t buf[3 * CHUNK + 16];
    memcpy(buf, rgb, static_cast<size_t>(3 * n));
    memset(buf + 3 * n, 255, sizeof(buf) - static_cast<size_t>(3 * n));
    tone_fx_srgb_to_lin(buf, tone_fx_pad8(n), s_ck.r, s_ck.g, s_ck.b);
}

// Out of line, with no constant argument propagated into the body, where the compiler has the
// attribute. The per pixel loops take the chunk's addresses as arguments, so they stay in registers.
#if __has_attribute(noipa)
#define NOIPA __attribute__((noipa))
#else
#define NOIPA __attribute__((noinline))
#endif
#define ALWAYS_INLINE inline __attribute__((always_inline))

typedef uint32_t __attribute__((may_alias)) word_alias;

// True when every byte of the n pixels at rgb is 255. Byte 255 alone decodes to Q15_ONE, so these
// are the chunks whose pixels all take the slot WHITE. Reads 16 bytes a step when rgb is word
// aligned and n a multiple of 16.
bool all_white(const uint8_t* rgb, int n)
{
    if((reinterpret_cast<uintptr_t>(rgb) & 3) == 0 && n % 16 == 0)
    {
        const word_alias* w = reinterpret_cast<const word_alias*>(rgb);
        for(int i = 0; i < 3 * n / 4; i += 4)
            if((w[i] & w[i + 1] & w[i + 2] & w[i + 3]) != 0xffffffffu) return false;
        return true;
    }
    for(int i = 0; i < 3 * n; i++)
        if(rgb[i] != 255) return false;
    return true;
}

// The sRGB decode of the n pixels at rgb into s_ck.r, g and b.
void decode(const uint8_t* rgb, int n)
{
    chunk& c = s_ck;
    if(n % 8 == 0) tone_fx_srgb_to_lin(rgb, n, c.r, c.g, c.b);
    else decode_short(rgb, n);
}

// The first step of pass A after the decode of n pixels into c: the slot of each pixel into
// c.slot. A white pixel takes the slot WHITE. A pixel with the linear light of the last pixel kept
// takes that pixel's slot. The pixels kept move down to the slots 0 to nu - 1. Returns nu, 0 when
// every pixel is white. A pixel that is not white is written to the slot nu whether it is kept or
// not: nu is at most its own index, and the slot nu is not yet in use.
NOIPA int compact(chunk& c, int n)
{
    uint16_t* const r  = reinterpret_cast<uint16_t*>(c.r);
    uint16_t* const g  = reinterpret_cast<uint16_t*>(c.g);
    uint16_t* const b  = reinterpret_cast<uint16_t*>(c.b);
    uint8_t* const  sl = c.slot;
    unsigned        lr = ~0u, lg = ~0u, lb = ~0u;
    int             nu = 0;
    for(int i = 0; i < n; i++)
    {
        // every decoded value is 0 to Q15_ONE
        unsigned vr = r[i], vg = g[i], vb = b[i];
        while((vr & vg & vb) == static_cast<unsigned>(Q15_ONE))
        {
            sl[i] = WHITE;
            if(++i == n) return nu;
            vr = r[i], vg = g[i], vb = b[i];
        }
        const unsigned diff = (vr ^ lr) | (vg ^ lg) | (vb ^ lb);
        r[nu]               = static_cast<uint16_t>(vr);
        g[nu]               = static_cast<uint16_t>(vg);
        b[nu]               = static_cast<uint16_t>(vb);
        nu += diff != 0;
        lr = vr, lg = vg, lb = vb;
        sl[i] = static_cast<uint8_t>(nu - 1);
    }
    return nu;
}

// Pass A over the slots 0 to nu - 1 of s_ck, whose r, g and b hold the decoded pixels. The kernels
// run on nu rounded up to 8, the padding lanes on black.
template <bool COUNT = false> void pass_a(int nu)
{
    chunk&    c = s_ck;
    const int n = tone_fx_pad8(nu);
    for(int i = nu; i < n; i++) c.r[i] = c.g[i] = c.b[i] = 0;
    tone_fx_lin_to_oklab(c.r, c.g, c.b, n, c.L, c.A, c.B);
    tone_fx_squeeze(c.L, c.A, c.B, n, c.L, c.A, c.B, c.C);
    bool colour = false, neutral = false;
    for(int i = 0; i < nu; i++)
    {
        if(c.C[i] < NEUTRAL_C_Q) neutral = true;
        else colour = true;
    }
    if(colour) tone_fx_oklab_to_lin(c.L, c.A, c.B, n, c.r, c.g, c.b);
    if(neutral) tone_fx_bracket(c.L, n, c.lo, c.hi);
    for(int i = 0; i < nu; i++)
    {
        c.ent[i] = NO_ENTRY;
        if(c.C[i] >= NEUTRAL_C_Q) c.kind[i] = K_COLOUR;
        else if(c.lo[i] == c.hi[i])
        {
            c.kind[i] = K_ENTRY;
            c.ent[i]  = c.lo[i];
        }
        else
        {
            c.kind[i] = K_NEUTRAL;
            c.r[i]    = static_cast<q15>(cube_q15(c.L[i]));
        }
    }
    BENCH_COUNT(kernel_chunks, 1);
    BENCH_COUNT(lanes, n);
    BENCH_COUNT(colour_chunks, colour);
    BENCH_COUNT(neutral_chunks, neutral);
}

// The entry for a pixel of slot s and kind K_NEUTRAL or K_COLOUR, with the clamped carried error ec
// and the noise nz. Writes the residual in linear light to res, and to near whether the search point
// lies within TONES_DITHER_HYBRID_NEAR of the entry.
ALWAYS_INLINE unsigned decide(int s, int kind, int nz, const int ec[3], int res[3], bool* near)
{
    const chunk& c = s_ck;
    if(kind == K_NEUTRAL)
    {
        // A neutral pixel mixes only the two neutral entries around its target, and the carried
        // error is held inside that pair. Neutral entries cannot pay a tint back, so the pixel
        // carries the luminance of the error alone.
        const unsigned lo  = c.lo[s], hi = c.hi[s];
        const int      lum = (LUMA_Q[0] * ec[0] + LUMA_Q[1] * ec[1] + LUMA_Q[2] * ec[2] + (1 << 14)) >> 15;
        const int      v   = clampi(c.r[s] + lum, s_qlin[lo][0], s_qlin[hi][0]);
        // The decision point: the pair's midpoint in Oklab L, moved by the noise, compared in linear
        // light as its cube.
        const int      m    = (s_pair_mid[hi] - s_pair_rel[hi] * nz + (1 << 8)) >> 9;
        const unsigned best = v > cube_q15(m) ? hi : lo;
        res[0] = res[1] = res[2] = v - s_qlin[best][0];
        if(HYBRID)
        {
            TONE_FX_ALIGNED q15 in[8] = {static_cast<q15>(v), static_cast<q15>(v), static_cast<q15>(v)};
            TONE_FX_ALIGNED q15 q[8];
            tone_fx_lin_to_oklab_1(in, q);
            *near = dist_q(q[0], 0, 0, best) < HYBRID_Q2;
        }
        return best;
    }
    // The search point: the target plus the error, held to 0 to 1, in Oklab.
    TONE_FX_ALIGNED q15 in[8];
    TONE_FX_ALIGNED q15 q[8];
    in[0] = static_cast<q15>(clampi(c.r[s] + ec[0], 0, Q15_ONE));
    in[1] = static_cast<q15>(clampi(c.g[s] + ec[1], 0, Q15_ONE));
    in[2] = static_cast<q15>(clampi(c.b[s] + ec[2], 0, Q15_ONE));
    tone_fx_lin_to_oklab_1(in, q);
    int pl = q[0];
    if(NOISE_L_Q != 0) pl = clampi(pl + ((NOISE_L_Q * nz + 64) >> 7), -32768, 32767);
    uint8_t b8, n8;
    tone_fx_search(static_cast<q15>(pl), q[1], q[2], &b8, &n8);
    unsigned best = b8;
    if(REL_Q > 0 && nz)
    {
        // Squared distances to the darker (lo) and lighter (hi) of the two nearest: the search point
        // lies past their midpoint by (d_lo - d_hi) / (2 |hi - lo|^2) of the gap.
        unsigned lo = b8, hi = n8;
        uint32_t dlo = dist_q(pl, 2 * q[1], 2 * q[2], lo), dhi = dist_q(pl, 2 * q[1], 2 * q[2], hi);
        if(s_qL[lo] > s_qL[hi]) std::swap(lo, hi), std::swap(dlo, dhi);
        const uint32_t dp = dist_q(s_qL[lo], s_q2a[lo], s_q2b[lo], hi);
        best = (static_cast<int64_t>(dlo) - static_cast<int64_t>(dhi)) * 262144 +
                       static_cast<int64_t>(REL_Q * nz) * dp >
                   0
                   ? hi
                   : lo;
    }
    for(int k = 0; k < 3; k++) res[k] = in[k] - s_qlin[best][k];
    if(KEEP_C_Q != 32768)
    {
        const int yl = (LUMA_Q[0] * res[0] + LUMA_Q[1] * res[1] + LUMA_Q[2] * res[2] + (1 << 14)) >> 15;
        for(int k = 0; k < 3; k++)
            res[k] = yl + static_cast<int>((static_cast<int64_t>(KEEP_C_Q) * (res[k] - yl) + (1 << 14)) >> 15);
    }
    if(HYBRID) *near = dist_q(q[0], 2 * q[1], 2 * q[2], best) < HYBRID_Q2;
    return best;
}

// The residual res of the pixel at x through the taps of kt, into the rows rp, scan direction d.
// Atkinson's taps take res / 8 each by share8(). The other kernels round the running sum of their
// weights, so their shares add to exactly res. A cell saturates at the q15 range.
void diffuse(const taps_q& kt, int x, int d, const int res[3], int16_t* const rp[3])
{
    int done[3] = {0, 0, 0}, cum = 0;
    for(int j = 0; j < kt.n; j++)
    {
        const tap_q& tp = kt.t[j];
        int16_t*     p  = rp[tp.dy] + (x + tp.dx * d + 2) * 3;
        cum += tp.n;
        for(int k = 0; k < 3; k++)
        {
            int sh;
            if(kt.den == 8) sh = share8(res[k]);
            else
            {
                const int at = round_div(static_cast<int64_t>(res[k]) * cum, kt.den);
                sh           = at - done[k];
                done[k]      = at;
            }
            p[k] = sat16(p[k] + sh);
        }
    }
}

// Pass B of the kernels other than FAST over the n pixels of the chunk at x0, scan direction d.
template <bool COUNT>
void pass_b_taps(int x0, int n, int d, int16_t* const rp[3], uint8_t* out, const int8_t* noise)
{
    const chunk&   c   = s_ck;
    const int16_t* cur = rp[0];
    for(int i = 0; i < n; i++)
    {
        const int x    = d > 0 ? x0 + i : x0 + n - 1 - i;
        const int s    = c.slot[x - x0];
        int       kind = c.kind[s];
        int       res[3];
        bool      err  = false; // the pixel leaves the residual res
        bool      near = false;
        if(kind == K_ENTRY) out[x] = c.lo[s];
        else
        {
            const int16_t* e = cur + (x + 2) * 3;
            int            ec[3];
            bool           none = true;
            for(int k = 0; k < 3; k++)
            {
                ec[k] = clampi(e[k], -ERR_MAX_Q, ERR_MAX_Q);
                none  = none && ec[k] >= -ERR_ZERO_Q && ec[k] <= ERR_ZERO_Q;
            }
            if(kind == K_WHITE)
            {
                if(none) kind = -1;
                else kind = s_white_kind;
            }
            if(kind == -1) out[x] = TONE_PAPER;
            else if(kind == K_ENTRY) out[x] = c.lo[s];
            else
            {
                out[x] = static_cast<uint8_t>(decide(s, kind, noise[x & (NOISE_N - 1)], ec, res, &near));
                err    = true;
                BENCH_COUNT(neutral_px, kind == K_NEUTRAL);
                BENCH_COUNT(colour_px, kind == K_COLOUR);
            }
        }
        if(err) diffuse(HYBRID && near ? k_taps[KERNEL_ATKINSON] : TAPS, x, d, res, rp);
    }
}

// The rows of pass B for one row of pixels.
struct pass_b_rows
{
    const int16_t* cur;
    int16_t*       row1;
    int16_t*       row2;
    uint8_t*       out;
    const int8_t*  noise;
};

// FAST: the error a pixel receives along the row is the share of the pixel before it plus the share
// of the one before that. Each pixel writes its share per channel to its own row2 cell, so the
// two row2 cells before a pixel hold both. The two cells before the first pixel of a row are
// cleared at the start of the row. A cell of row1 is complete when the pixel past it has run, so a
// pixel writes the row1 cell before it once, with the error along the row and its own share. A
// cell of row2 gets one share, so a pixel writes its own cell with no read and the row needs no
// clear. pend counts the pixels with no share after which the two row2 cells before the next pixel
// are 0: it is 0 only when they are. While it is 0, a pixel of a K_ENTRY slot only writes its entry
// and clears its row2 cell.
//
// Where pass B is in a chunk. pa is the lowest in memory of the row2 cells of the pixel (OWN) and of
// the two before it (A1, A2), so every access to them is at an offset of 0 or more. p1 is the row1
// cell before the pixel.
struct pass_b_pos
{
    const uint8_t* sp; // the pixel's slot
    uint8_t*       op; // the pixel's entry in out
    int16_t*       pa;
    int16_t*       p1;
    int            k; // the pixels left, this one included
    int            pend;
};

template <int D> struct cells
{
    static const int OWN = D > 0 ? 6 : 0, A1 = 3, A2 = D > 0 ? 0 : 6;
};

// decide() of a K_NEUTRAL pixel of slot s, with the clamped carried error er, eg and eb and the noise
// nz. Writes the entry to *o and returns the share of each channel, the same in all three.
inline int neutral_step(int s, int er, int eg, int eb, int nz, uint8_t* o)
{
    const chunk&   c    = s_ck;
    const unsigned lo   = c.lo[s], hi = c.hi[s];
    const int      lum  = (LUMA_Q[0] * er + LUMA_Q[1] * eg + LUMA_Q[2] * eb + (1 << 14)) >> 15;
    const int      v    = clampi(c.r[s] + lum, s_qlin[lo][0], s_qlin[hi][0]);
    const int      m    = (s_pair_mid[hi] - s_pair_rel[hi] * nz + (1 << 8)) >> 9;
    const unsigned best = v > cube_q15(m) ? hi : lo;
    *o                  = static_cast<uint8_t>(best);
    return share8(v - s_qlin[best][0]);
}

// FAST: pass B from ps on while the pixels are K_ENTRY or K_NEUTRAL. Stops at the end of the chunk
// or at a pixel of another kind, which run_c() takes. The loop calls nothing, so its values stay
// in registers. ent is s_ck.ent, passed in to stay in a register too.
template <bool COUNT, int D>
NOIPA void run_b(pass_b_pos& ps, const uint8_t* ent, const pass_b_rows& rw)
{
    typedef cells<D> L;
    const chunk&   c    = s_ck;
    const uint8_t* sp   = ps.sp;
    uint8_t*       op   = ps.op;
    int16_t*       pa   = ps.pa;
    int16_t*       p1   = ps.p1;
    int            k    = ps.k;
    int            pend = ps.pend;
    for(; k > 0; k--, sp += D, op += D, pa += 3 * D, p1 += 3 * D)
    {
        if(pend == 0)
        {
            for(; k > 0; k--, sp += D, op += D, pa += 3 * D, p1 += 3 * D)
            {
                const int e = ent[*sp];
                if(e & 0x80) break;
                *op           = static_cast<uint8_t>(e);
                pa[L::OWN]     = 0;
                pa[L::OWN + 1] = 0;
                pa[L::OWN + 2] = 0;
                BENCH_COUNT(entry_run_px, 1);
                BENCH_COUNT(white_run_px, *sp == WHITE);
            }
            if(k == 0) break;
        }
        const int s = *sp;
        const int e = ent[s];
        // the error into the pixel along the row
        const int sr = pa[L::A1] + pa[L::A2];
        const int sg = pa[L::A1 + 1] + pa[L::A2 + 1];
        const int sb = pa[L::A1 + 2] + pa[L::A2 + 2];
        int       h  = 0;
        if(e & 0x80)
        {
            if(c.kind[s] != K_NEUTRAL) break;
            const int      x  = static_cast<int>(op - rw.out);
            const int16_t* ep = rw.cur + (x + 2) * 3;
            h = neutral_step(s, clampi(ep[0] + sr, -ERR_MAX_Q, ERR_MAX_Q), clampi(ep[1] + sg, -ERR_MAX_Q, ERR_MAX_Q),
                             clampi(ep[2] + sb, -ERR_MAX_Q, ERR_MAX_Q), rw.noise[x & (NOISE_N - 1)], op);
            BENCH_COUNT(neutral_px, 1);
        }
        else
        {
            *op = static_cast<uint8_t>(e);
            BENCH_COUNT(entry_carry_px, 1);
        }
        p1[0]          = static_cast<int16_t>(p1[0] + sr + h);
        p1[1]          = static_cast<int16_t>(p1[1] + sg + h);
        p1[2]          = static_cast<int16_t>(p1[2] + sb + h);
        pa[L::OWN]     = static_cast<int16_t>(h);
        pa[L::OWN + 1] = static_cast<int16_t>(h);
        pa[L::OWN + 2] = static_cast<int16_t>(h);
        pend           = h ? 2 : std::max(pend - 1, 0);
    }
    ps.sp   = sp;
    ps.op   = op;
    ps.pa   = pa;
    ps.p1   = p1;
    ps.k    = k;
    ps.pend = pend;
}

// FAST: pass B of the pixel at ps, of kind K_COLOUR or K_WHITE, then ps moves to the next pixel.
template <bool COUNT, int D> ALWAYS_INLINE void step_c(pass_b_pos& ps, const pass_b_rows& rw)
{
    typedef cells<D> L;
    const chunk&   c  = s_ck;
    int16_t* const pa = ps.pa;
    int16_t* const p1 = ps.p1;
    uint8_t* const op = ps.op;
    const int      s  = *ps.sp;
    const int      sr = pa[L::A1] + pa[L::A2];
    const int      sg = pa[L::A1 + 1] + pa[L::A2 + 1];
    const int      sb = pa[L::A1 + 2] + pa[L::A2 + 2];
    const int      x  = static_cast<int>(op - rw.out);
    const int16_t* ep = rw.cur + (x + 2) * 3;
    const int      er = clampi(ep[0] + sr, -ERR_MAX_Q, ERR_MAX_Q);
    const int      eg = clampi(ep[1] + sg, -ERR_MAX_Q, ERR_MAX_Q);
    const int      eb = clampi(ep[2] + sb, -ERR_MAX_Q, ERR_MAX_Q);
    int            kind = c.kind[s];
    if(kind == K_WHITE)
    {
        const bool none = er >= -ERR_ZERO_Q && er <= ERR_ZERO_Q && eg >= -ERR_ZERO_Q && eg <= ERR_ZERO_Q &&
                          eb >= -ERR_ZERO_Q && eb <= ERR_ZERO_Q;
        kind = none ? -1 : s_white_kind;
    }
    const int nz = rw.noise[x & (NOISE_N - 1)];
    int       hr = 0, hg = 0, hb = 0;
    if(kind == K_NEUTRAL)
    {
        hr = hg = hb = neutral_step(s, er, eg, eb, nz, op);
        BENCH_COUNT(neutral_px, 1);
    }
    else if(kind == K_COLOUR)
    {
        const int ec[3] = {er, eg, eb};
        int       res[3];
        bool      near = false;
        *op            = static_cast<uint8_t>(decide(s, K_COLOUR, nz, ec, res, &near));
        hr             = share8(res[0]);
        hg             = share8(res[1]);
        hb             = share8(res[2]);
        BENCH_COUNT(colour_px, 1);
        BENCH_COUNT(colour_two_px, REL_Q > 0 && nz != 0);
    }
    else
    {
        *op = kind == K_ENTRY ? c.lo[s] : TONE_PAPER;
        BENCH_COUNT(entry_carry_px, 1);
    }
    p1[0]          = static_cast<int16_t>(p1[0] + sr + hr);
    p1[1]          = static_cast<int16_t>(p1[1] + sg + hg);
    p1[2]          = static_cast<int16_t>(p1[2] + sb + hb);
    pa[L::OWN]     = static_cast<int16_t>(hr);
    pa[L::OWN + 1] = static_cast<int16_t>(hg);
    pa[L::OWN + 2] = static_cast<int16_t>(hb);
    ps.pend        = (hr | hg | hb) ? 2 : std::max(ps.pend - 1, 0);
    ps.sp += D;
    ps.op += D;
    ps.pa += 3 * D;
    ps.p1 += 3 * D;
    ps.k--;
}

// FAST: pass B from ps on while the pixels are of kind K_COLOUR or K_WHITE. The pixel at ps is.
template <bool COUNT, int D> NOIPA void run_c(pass_b_pos& ps, const pass_b_rows& rw)
{
    const chunk& c = s_ck;
    pass_b_pos   p = ps;
    BENCH_COUNT(colour_runs, 1);
    do
    {
        step_c<COUNT, D>(p, rw);
    } while(p.k > 0 && (c.ent[*p.sp] & 0x80) && c.kind[*p.sp] != K_NEUTRAL);
    ps = p;
}

// Pass B of FAST over the n pixels of the chunk at x0, scan direction D. Returns pend.
template <bool COUNT, int D> int pass_b_fast(const pass_b_rows& rw, int x0, int n, int pend)
{
    const int  i0 = D > 0 ? 0 : n - 1;
    pass_b_pos ps = {s_ck.slot + i0, rw.out + x0 + i0, rw.row2 + (x0 + i0 + (D > 0 ? 0 : 2)) * 3,
                     rw.row1 + (x0 + i0 - D + 2) * 3, n, pend};
    for(;;)
    {
        run_b<COUNT, D>(ps, s_ck.ent, rw);
        if(ps.k == 0) return ps.pend;
        run_c<COUNT, D>(ps, rw);
    }
}

// FAST: the row1 cell before the pixel at x, completed as by a pixel at x with no share.
void flush(int16_t* row1, const int16_t* row2, int x, int d)
{
    int16_t*       p1 = row1 + (x - d + 2) * 3;
    const int16_t* p2 = row2 + (x + 2) * 3;
    for(int k = 0; k < 3; k++) p1[k] = static_cast<int16_t>(p1[k] + p2[k - 3 * d] + p2[k - 6 * d]);
}

// One row. See the top of this file.
template <bool COUNT> void dither_row(tone_dither& dt, const uint8_t* rgb, int y, uint8_t* out)
{
    const int      w     = dt.w;
    int16_t*       cur   = dt.rows[0];
    int16_t*       row1  = dt.rows[1];
    int16_t*       row2  = dt.rows[2];
    int16_t* const rp[3] = {cur, row1, row2};
    const int8_t*  noise = s_noise[y & (NOISE_N - 1)].data();
    const int      d     = (y & 1) ? -1 : 1;
    const int      nch   = (w + CHUNK - 1) / CHUNK;
    chunk&         c     = s_ck;
    int            pend  = 0;
    const pass_b_rows rw = {cur, row1, row2, out, noise};
    // FAST: the two row2 cells before the first pixel, the shares of no pixel
    if(FAST) memset(row2 + (d > 0 ? 0 : (w + 2) * 3), 0, 6 * sizeof(int16_t));
    for(int ci = 0; ci < nch; ci++)
    {
        BENCH_NOW(t0);
        const int      x0    = (d > 0 ? ci : nch - 1 - ci) * CHUNK;
        const int      n     = std::min(w, x0 + CHUNK) - x0;
        const uint8_t* src   = rgb + 3 * x0;
        const bool     white = all_white(src, n);
        if(!white) decode(src, n);
        BENCH_NOW(t1);
        const int nu = white ? 0 : compact(c, n);
        BENCH_NOW(t2);
        if(nu) pass_a<COUNT>(nu);
        BENCH_NOW(t3);
        BENCH_COUNT(chunks, 1);
        if(!nu && c.kind[WHITE] == K_ENTRY)
        {
            // every pixel white, and white on an entry: no pixel reads its error or leaves one
            memset(out + x0, c.lo[WHITE], static_cast<size_t>(n));
            if(FAST)
            {
                const int xf = d > 0 ? x0 : x0 + n - 1;
                memset(row2 + (x0 + 2) * 3, 0, static_cast<size_t>(n) * 3 * sizeof(int16_t));
                flush(row1, row2, xf, d);
                if(n > 1) flush(row1, row2, xf + d, d);
                pend = n > 1 ? 0 : std::max(pend - 1, 0);
            }
            BENCH_COUNT(white_chunks, 1);
            BENCH_COUNT(white_chunk_px, n);
        }
        else
        {
            // with white off its entry, a chunk of white pixels runs pass B on the slot WHITE
            if(!nu) memset(c.slot, WHITE, static_cast<size_t>(n));
            if(!FAST) pass_b_taps<COUNT>(x0, n, d, rp, out, noise);
            else if(d > 0) pend = pass_b_fast<COUNT, 1>(rw, x0, n, pend);
            else pend = pass_b_fast<COUNT, -1>(rw, x0, n, pend);
        }
        BENCH_NOW(t4);
        BENCH_ADD(PH_DECODE, t1, t0);
        BENCH_ADD(PH_COMPACT, t2, t1);
        BENCH_ADD(PH_KERNELS, t3, t2);
        BENCH_ADD(PH_PASS_B, t4, t3);
    }
    // the row1 cell of the last pixel, which no pixel past it completes
    if(FAST && w > 0) flush(row1, row2, d > 0 ? w : -1, d);
    dt.rows[0] = row1;
    dt.rows[1] = row2;
    dt.rows[2] = cur;
    if(!FAST) memset(cur, 0, static_cast<size_t>(w + 4) * 3 * sizeof(int16_t));
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

#if TONES_HAVE_FLOAT_REF
#include "tones_float_ref.inc"
#endif

// The tables are data (tone_tables.h). What runs here: the boot check of the expanded patterns,
// which cannot be skipped, the copy of the slot WHITE into the chunk, and the frame.
bool tones_init()
{
    char why[192];
    if(!tone_plan_init(why, sizeof(why)))
    {
        ESP_LOGE(TAG, "tone map refused: %s", why);
        return false;
    }

    // The slot WHITE as pass A leaves it for a white pixel, computed on the host by the kernels the
    // dither runs. The kernels write at most CHUNK slots, so the chunk keeps it.
    chunk&                 c = s_ck;
    const tone_white_slot& w = tone_tables::white;
    c.r[WHITE] = w.r, c.g[WHITE] = w.g, c.b[WHITE] = w.b;
    c.L[WHITE] = w.L, c.A[WHITE] = w.A, c.B[WHITE] = w.B, c.C[WHITE] = w.C;
    c.lo[WHITE] = w.lo, c.hi[WHITE] = w.hi, c.kind[WHITE] = w.kind, c.ent[WHITE] = w.ent;
    if(w.kind == K_WHITE)
        ESP_LOGW(TAG, "white is not on the paper entry: kind %u, entries %u %u", (unsigned)w.white_kind,
                 (unsigned)w.lo, (unsigned)w.hi);

    // In PSRAM: the browser task needs its stack as one block of internal RAM.
    if(!s_frame)
        s_frame = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(TONE_W) * TONE_H, MALLOC_CAP_SPIRAM));
    if(!s_frame)
    {
        ESP_LOGE(TAG, "no PSRAM for the frame");
        return false;
    }
    memset(s_frame, TONE_PAPER, static_cast<size_t>(TONE_W) * TONE_H);
#if TONES_HAVE_FLOAT_REF
    if(!float_ref_init()) return false;
#endif
    ESP_LOGI(TAG, "%u entries, %d neutral, black entry %u", N, tone_tables::neutral_count, (unsigned)s_black);
    return true;
}

uint8_t tones_black()
{
    return s_black;
}

// The table of main/tone_tables.inc, built on the host by pass A of the kernels on every gray byte.
int tones_solid_gray(int g)
{
    return tone_tables::solid_gray[static_cast<size_t>(std::max(0, std::min(255, g)))];
}

uint8_t* tones_frame()
{
    return s_frame;
}

// ---------------------------------------------------------------- dither

void tone_dither::alloc(int w_)
{
    release();
    w              = w_;
    const size_t n = static_cast<size_t>(w + 4) * 3;
    const size_t b = 3 * n * sizeof(int16_t);
    // Internal RAM first: pass B reads and writes every cell of the three rows on every row.
    block = static_cast<int16_t*>(heap_caps_malloc(b, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if(!block) block = static_cast<int16_t*>(heap_caps_malloc(b, MALLOC_CAP_8BIT));
    if(!block)
    {
        ESP_LOGE(TAG, "no memory for the error rows of %d pixels", w);
        return;
    }
    memset(block, 0, b);
    rows[0] = block;
    rows[1] = block + n;
    rows[2] = block + 2 * n;
}

void tone_dither::release()
{
    heap_caps_free(block);
    block   = nullptr;
    rows[0] = rows[1] = rows[2] = nullptr;
}

void tone_dither::row(const uint8_t* c, int y, uint8_t* out)
{
    if(!block)
    {
        memset(out, TONE_PAPER, static_cast<size_t>(w));
        return;
    }
#ifdef TONES_BENCH
    if(s_bench_count)
    {
        dither_row<true>(*this, c, y, out);
        return;
    }
#endif
    dither_row<false>(*this, c, y, out);
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
