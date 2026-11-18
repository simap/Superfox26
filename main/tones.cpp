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

static const int ERR_MAX_Q = 8192;
static const int ERR_ZERO_Q = 3;
static const int NEUTRAL_C_Q = 33;
static const int LUMA_Q[3] = {6966, 23436, 2366};
static const float STOCK_RED_C   = 0.09f;
static const float STOCK_BLACK_L = 0.5f;

static const int NOISE_N = 64;
static constexpr float NOISE_REL = 0.25f;

namespace
{
const unsigned N = static_cast<unsigned>(TONEMAP_COUNT);

const int REL_Q = static_cast<int>(NOISE_REL * 4096.0f + 0.5f);

static_assert(static_cast<size_t>(TONE_W) * TONE_H == static_cast<size_t>(EPD_PLANE_BYTES) * 8, "frame and planes differ");
static_assert(TONE_W == EPD_W && TONE_H == EPD_H, "frame and panel differ");
static_assert(TONEMAP_COUNT <= 128, "a slot of kind K_ENTRY holds the entry in a byte below NO_ENTRY");

enum slot_kind : uint8_t
{
    K_ENTRY,
    K_NEUTRAL,
    K_COLOUR,
    K_WHITE,
};

const int CHUNK = 160;
const int SLOTS = CHUNK + 16;
const int WHITE = CHUNK;
static_assert(CHUNK % 8 == 0 && SLOTS % 16 == 0 && WHITE < 256, "chunk layout");
const uint8_t NO_ENTRY = 0xff;

struct chunk
{
    TONE_FX_ALIGNED q15 r[SLOTS];
    TONE_FX_ALIGNED q15 g[SLOTS];
    TONE_FX_ALIGNED q15 b[SLOTS];
    TONE_FX_ALIGNED q15 L[SLOTS];
    TONE_FX_ALIGNED q15 A[SLOTS];
    TONE_FX_ALIGNED q15 B[SLOTS];
    TONE_FX_ALIGNED q15 C[SLOTS];
    TONE_FX_ALIGNED uint8_t lo[SLOTS];
    TONE_FX_ALIGNED uint8_t hi[SLOTS];
    TONE_FX_ALIGNED uint8_t kind[SLOTS];
    TONE_FX_ALIGNED uint8_t ent[SLOTS];
    uint8_t                 slot[CHUNK];
};

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

constexpr bool lin_fits_error_rows()
{
    int lin_min = 0, lin_max = 0;
    for(const auto& lin : tone_tables::qlin)
        for(int v : lin) lin_min = std::min(lin_min, v), lin_max = std::max(lin_max, v);
    return (std::max(Q15_ONE - lin_min, lin_max) * 6 + 7) / 8 + 8 <= 32767;
}
static_assert(lin_fits_error_rows(), "an entry's linear light runs past the error range");

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
            static_cast<int32_t>(static_cast<double>(NOISE_REL) / 128.0 * (lh - ll) * 16777216.0 + 0.5);
    }
    return p;
}

typedef std::array<std::array<int8_t, NOISE_N>, NOISE_N> noise_table;

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
const uint8_t s_white_kind = tone_tables::white.white_kind;

TONE_TABLES_RAM std::array<int16_t, TONEMAP_COUNT>                s_qL       = tone_tables::qL;
TONE_TABLES_RAM std::array<int16_t, TONEMAP_COUNT>                s_q2a      = tone_tables::q2a;
TONE_TABLES_RAM std::array<int16_t, TONEMAP_COUNT>                s_q2b      = tone_tables::q2b;
TONE_TABLES_RAM std::array<std::array<int16_t, 3>, TONEMAP_COUNT> s_qlin     = tone_tables::qlin;
TONE_TABLES_RAM std::array<int32_t, TONEMAP_COUNT>                s_pair_mid = make_pairs().mid;
TONE_TABLES_RAM std::array<int32_t, TONEMAP_COUNT>                s_pair_rel = make_pairs().rel;
TONE_TABLES_RAM noise_table s_noise = tone_tables::noise_blue;
const lab_table s_lab   = make_lab();
const uint8_t   s_black = static_cast<uint8_t>(entry_of(TONEMAP_BLACK_PATTERN));
uint8_t*        s_frame     = nullptr;
bool            s_tables_ok = true;

#ifdef TONES_BENCH
enum bench_phase
{
    PH_DECODE,
    PH_COMPACT,
    PH_KERNELS,
    PH_PASS_B,
    PH_N
};
struct bench_counts
{
    uint32_t chunks, white_chunks, kernel_chunks, lanes, colour_chunks, neutral_chunks;
    uint32_t white_chunk_px;
    uint32_t entry_run_px;
    uint32_t white_run_px;
    uint32_t entry_carry_px;
    uint32_t neutral_px, colour_px;
    uint32_t colour_two_px;
    uint32_t colour_runs;
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

inline int cube_q15(int l)
{
    return static_cast<int>((static_cast<int64_t>(l * l) * l + (1 << 29)) >> 30);
}

inline uint32_t dist_q(int l, int a2, int b2, unsigned e)
{
    const uint32_t dl = static_cast<uint32_t>(l - s_qL[e]);
    const uint32_t da = static_cast<uint32_t>(a2 - s_q2a[e]);
    const uint32_t db = static_cast<uint32_t>(b2 - s_q2b[e]);
    return dl * dl + da * da + db * db;
}

inline int share8(int res)
{
    return (res + 3 + ((res >> 3) & 1)) >> 3;
}

__attribute__((noinline)) void decode_short(const uint8_t* rgb, int n)
{
    uint8_t buf[3 * CHUNK + 16];
    memcpy(buf, rgb, static_cast<size_t>(3 * n));
    memset(buf + 3 * n, 255, sizeof(buf) - static_cast<size_t>(3 * n));
    tone_fx_srgb_to_lin(buf, tone_fx_pad8(n), s_ck.r, s_ck.g, s_ck.b);
}

#if __has_attribute(noipa)
#define NOIPA __attribute__((noipa))
#else
#define NOIPA __attribute__((noinline))
#endif
#define ALWAYS_INLINE inline __attribute__((always_inline))

typedef uint32_t __attribute__((may_alias)) word_alias;

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

void decode(const uint8_t* rgb, int n)
{
    chunk& c = s_ck;
    if(n % 8 == 0) tone_fx_srgb_to_lin(rgb, n, c.r, c.g, c.b);
    else decode_short(rgb, n);
}

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

ALWAYS_INLINE unsigned decide_colour(int s, int nz, const int ec[3], int res[3])
{
    const chunk& c = s_ck;
    TONE_FX_ALIGNED q15 in[8];
    TONE_FX_ALIGNED q15 q[8];
    in[0] = static_cast<q15>(clampi(c.r[s] + ec[0], 0, Q15_ONE));
    in[1] = static_cast<q15>(clampi(c.g[s] + ec[1], 0, Q15_ONE));
    in[2] = static_cast<q15>(clampi(c.b[s] + ec[2], 0, Q15_ONE));
    tone_fx_lin_to_oklab_1(in, q);
    const int pl = q[0];
    uint8_t   b8, n8;
    tone_fx_search(static_cast<q15>(pl), q[1], q[2], &b8, &n8);
    unsigned best = b8;
    if(REL_Q > 0 && nz)
    {
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
    return best;
}

struct pass_b_rows
{
    const int16_t* cur;
    int16_t*       row1;
    int16_t*       row2;
    uint8_t*       out;
    const int8_t*  noise;
};

struct pass_b_pos
{
    const uint8_t* sp;
    uint8_t*       op;
    int16_t*       pa;
    int16_t*       p1;
    int            k;
    int            pend;
};

template <int D> struct cells
{
    static const int OWN = D > 0 ? 6 : 0, A1 = 3, A2 = D > 0 ? 0 : 6;
};

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

template <bool COUNT, int D>
NOIPA void run_b(pass_b_pos& ps, const uint8_t* ent, const pass_b_rows& rw)
{
    typedef cells<D> C;
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
                pa[C::OWN]     = 0;
                pa[C::OWN + 1] = 0;
                pa[C::OWN + 2] = 0;
                BENCH_COUNT(entry_run_px, 1);
                BENCH_COUNT(white_run_px, *sp == WHITE);
            }
            if(k == 0) break;
        }
        const int s = *sp;
        const int e = ent[s];
        const int sr = pa[C::A1] + pa[C::A2];
        const int sg = pa[C::A1 + 1] + pa[C::A2 + 1];
        const int sb = pa[C::A1 + 2] + pa[C::A2 + 2];
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
        pa[C::OWN]     = static_cast<int16_t>(h);
        pa[C::OWN + 1] = static_cast<int16_t>(h);
        pa[C::OWN + 2] = static_cast<int16_t>(h);
        pend           = h ? 2 : std::max(pend - 1, 0);
    }
    ps.sp   = sp;
    ps.op   = op;
    ps.pa   = pa;
    ps.p1   = p1;
    ps.k    = k;
    ps.pend = pend;
}

template <bool COUNT, int D> ALWAYS_INLINE void step_c(pass_b_pos& ps, const pass_b_rows& rw)
{
    typedef cells<D> C;
    const chunk&   c  = s_ck;
    int16_t* const pa = ps.pa;
    int16_t* const p1 = ps.p1;
    uint8_t* const op = ps.op;
    const int      s  = *ps.sp;
    const int      sr = pa[C::A1] + pa[C::A2];
    const int      sg = pa[C::A1 + 1] + pa[C::A2 + 1];
    const int      sb = pa[C::A1 + 2] + pa[C::A2 + 2];
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
        *op             = static_cast<uint8_t>(decide_colour(s, nz, ec, res));
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
    pa[C::OWN]     = static_cast<int16_t>(hr);
    pa[C::OWN + 1] = static_cast<int16_t>(hg);
    pa[C::OWN + 2] = static_cast<int16_t>(hb);
    ps.pend        = (hr | hg | hb) ? 2 : std::max(ps.pend - 1, 0);
    ps.sp += D;
    ps.op += D;
    ps.pa += 3 * D;
    ps.p1 += 3 * D;
    ps.k--;
}

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

template <bool COUNT, int D> int pass_b_chunk(const pass_b_rows& rw, int x0, int n, int pend)
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

void flush(int16_t* row1, const int16_t* row2, int x, int d)
{
    int16_t*       p1 = row1 + (x - d + 2) * 3;
    const int16_t* p2 = row2 + (x + 2) * 3;
    for(int k = 0; k < 3; k++) p1[k] = static_cast<int16_t>(p1[k] + p2[k - 3 * d] + p2[k - 6 * d]);
}

template <bool COUNT> void dither_row(tone_dither& dt, const uint8_t* rgb, int y, uint8_t* out)
{
    const int      w     = dt.w;
    int16_t*       cur   = dt.rows[0];
    int16_t*       row1  = dt.rows[1];
    int16_t*       row2  = dt.rows[2];
    const int8_t*  noise = s_noise[y & (NOISE_N - 1)].data();
    const int      d     = (y & 1) ? -1 : 1;
    const int      nch   = (w + CHUNK - 1) / CHUNK;
    chunk&         c     = s_ck;
    int            pend  = 0;
    const pass_b_rows rw = {cur, row1, row2, out, noise};
    memset(row2 + (d > 0 ? 0 : (w + 2) * 3), 0, 6 * sizeof(int16_t));
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
            memset(out + x0, c.lo[WHITE], static_cast<size_t>(n));
            const int xf = d > 0 ? x0 : x0 + n - 1;
            memset(row2 + (x0 + 2) * 3, 0, static_cast<size_t>(n) * 3 * sizeof(int16_t));
            flush(row1, row2, xf, d);
            if(n > 1) flush(row1, row2, xf + d, d);
            pend = n > 1 ? 0 : std::max(pend - 1, 0);
            BENCH_COUNT(white_chunks, 1);
            BENCH_COUNT(white_chunk_px, n);
        }
        else
        {
            if(!nu) memset(c.slot, WHITE, static_cast<size_t>(n));
            if(d > 0) pend = pass_b_chunk<COUNT, 1>(rw, x0, n, pend);
            else pend = pass_b_chunk<COUNT, -1>(rw, x0, n, pend);
        }
        BENCH_NOW(t4);
        BENCH_ADD(PH_DECODE, t1, t0);
        BENCH_ADD(PH_COMPACT, t2, t1);
        BENCH_ADD(PH_KERNELS, t3, t2);
        BENCH_ADD(PH_PASS_B, t4, t3);
    }
    if(w > 0) flush(row1, row2, d > 0 ? w : -1, d);
    dt.rows[0] = row1;
    dt.rows[1] = row2;
    dt.rows[2] = cur;
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

bool draw_stock(const uint8_t* frame)
{
    uint8_t code[TONEMAP_COUNT];
    for(unsigned e = 0; e < N; e++)
        code[e] = hypotf(s_lab[e][1], s_lab[e][2]) > STOCK_RED_C ? 2 : s_lab[e][0] < STOCK_BLACK_L ? 1 : 0;
    build_planes(frame, code);
    const bool ok = refresh_display(EPD_SLOW);
    epd_sleep();
    return ok;
}

esp_err_t session_open(tone_phase ph)
{
    const epd_tone_table_t table = ph == TONE_PHASE_1 ? EPD_TONE_TABLE_PHASE1 : EPD_TONE_TABLE_SEQ;
    const uint8_t          unit  = ph == TONE_PHASE_1 ? 1 : TONE_NUDGE_FRAMES;
    if(epd_tone_is_open() && epd_tone_table() == table && (table != EPD_TONE_TABLE_SEQ || epd_tone_unit() == unit))
        return ESP_OK;
    return epd_tone_open_table(unit, 0, table);
}

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
            if(repeat) err = epd_tone_pull(1, false, &ms);
            else
            {
                build_planes(frame, code);
                err = epd_tone_pass(1, &ms);
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
}

#if TONES_HAVE_FLOAT_REF
#include "tones_float_ref.inc"
#endif

bool tones_init()
{
    char why[192];
    if(!tone_plan_init(why, sizeof(why)))
    {
        ESP_LOGE(TAG, "tone map refused: %s", why);
        return false;
    }

    chunk&                 c = s_ck;
    const tone_white_slot& w = tone_tables::white;
    c.r[WHITE] = w.r, c.g[WHITE] = w.g, c.b[WHITE] = w.b;
    c.L[WHITE] = w.L, c.A[WHITE] = w.A, c.B[WHITE] = w.B, c.C[WHITE] = w.C;
    c.lo[WHITE] = w.lo, c.hi[WHITE] = w.hi, c.kind[WHITE] = w.kind, c.ent[WHITE] = w.ent;
    if(w.kind == K_WHITE)
        ESP_LOGW(TAG, "white is not on the paper entry: kind %u, entries %u %u", (unsigned)w.white_kind,
                 (unsigned)w.lo, (unsigned)w.hi);

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

int tones_solid_gray(int g)
{
    return tone_tables::solid_gray[static_cast<size_t>(std::max(0, std::min(255, g)))];
}

uint8_t* tones_frame()
{
    return s_frame;
}

void tone_dither::alloc(int w_)
{
    release();
    w              = w_;
    const size_t n = static_cast<size_t>(w + 4) * 3;
    const size_t b = 3 * n * sizeof(int16_t);
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
    if(s_tables_ok)
    {
        const esp_err_t err = play(frame, count, st);
        epd_sleep();
        epd_clear();
        if(err == ESP_ERR_NOT_SUPPORTED)
        {
            ESP_LOGW(TAG, "panel has no tables, drawing the three stock colours");
            s_tables_ok = false;
        }
        ok = err == ESP_OK;
    }
    if(!s_tables_ok) ok = draw_stock(frame);
    st->total_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "draw %s: %u entries, %u + %u pulls, %u sends, open %u ms, pulls %u ms, total %u ms",
             ok ? "ok" : "failed", (unsigned)st->entries, (unsigned)st->pulls1, (unsigned)st->pulls2,
             (unsigned)st->sends, (unsigned)st->open_ms, (unsigned)st->pull_ms, (unsigned)st->total_ms);
    return ok;
}

#ifdef TONES_BENCH
#include "tones_bench.inc"
#endif
