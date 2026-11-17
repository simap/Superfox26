// The float dither, the reference for the fixed point dither of tones.cpp. tones.cpp includes this
// file when TONES_HAVE_FLOAT_REF is 1 (tones.h): the host harness and the benchmark. It holds the
// same rules in 32 bit float with libm. A pixel's target comes from a cache keyed by its RGB, and a
// coloured pixel searches the NB nearest neighbours of its target's nearest entry. It reads s_lab,
// s_black and s_noise of tones.cpp. tones_init() calls float_ref_init().

namespace
{
// Carried error limit per channel in linear light, as a share of the paper's light.
const float ERR_MAX = 0.25f;
// A carried error below this in every channel counts as none.
const float ERR_ZERO = 1e-4f;
// A target whose a, b chroma after the squeeze is below this is neutral. Float rounding leaves a
// gray's chroma near 1e-7, and the squeeze sets a blue or a green to exactly 0.
const float NEUTRAL_C = 1e-3f;
// A neutral target within this Oklab L of a neutral entry is on that entry. The table gives L to 4
// decimals, and in the midtones one gray byte is 0.003 to 0.006 in L from the next. At 0.002 a gray
// byte lands on 20 of the 22 neutral entries. It misses L 0.676, which lies 0.002 below the entry at
// L 0.678, and L 0.058, between bytes 0 and 1.
const float ON_ENTRY_L = 0.002f;
// Candidates searched per coloured pixel: the nearest entry to its target without error, and that
// entry's nearest neighbours in the weighted Oklab distance, itself first. On the host harness,
// against a search of all entries, 24 candidates change 6 to 13% of the pixels of the red ramp and
// the colour wedges and leave seams in the wedges, 48 change 0.3% at most.
const int NB = 48;
// Targets kept by RGB, 2^CACHE_BITS slots in PSRAM.
const int CACHE_BITS = 10;
// Luminance of linear sRGB.
const float LUMA[3] = {0.2126f, 0.7152f, 0.0722f};

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

// Indexed by dither_kernel.
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

float   s_lin[TONEMAP_COUNT][3]; // entry linear light
uint8_t s_nb[TONEMAP_COUNT][NB];
uint8_t s_by_l[TONEMAP_COUNT];      // all entries, darkest first, for the nearest entry search
uint8_t s_neutrals[TONEMAP_COUNT];  // the neutral entries, lightest first
float   s_neutral_l[TONEMAP_COUNT]; // and their L
int     s_n_neutral = 0;
float   s_byte_lin[256]; // sRGB byte to linear
target* s_cache = nullptr; // CACHE_SLOTS targets in PSRAM
#ifdef TONES_BENCH
int      s_bench_bits = CACHE_BITS; // cache index bits for tones_bench(), 0 for no cache
uint32_t s_bench_miss = 0, s_bench_look = 0;
#endif

// Linear sRGB to Oklab.
inline void lin_to_oklab(const float c[3], float out[3])
{
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

// Oklab to linear sRGB.
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
        const float gu  = up < n ? s_lab[s_by_l[up]][0] - t[0] : 1e18f;
        const float gd  = dn >= 0 ? t[0] - s_lab[s_by_l[dn]][0] : 1e18f;
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
    target& t = s_cache[(key * 0x9e3779b1u) >> (32 - CACHE_BITS)];
#endif
    if(t.key != key)
    {
        make_target(r, g, b, &t);
        t.key = key;
    }
    return t;
}

// The tables of the float dither. s_lab and s_black are set.
bool float_ref_init()
{
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        s_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    s_n_neutral = 0;
    for(unsigned e = 0; e < N; e++)
    {
        oklab_to_lin(s_lab[e], s_lin[e]);
        s_by_l[e] = static_cast<uint8_t>(e);
        if(strpbrk(k_tonemap[e].pattern, "rR")) continue;
        s_lin[e][0] = s_lin[e][1] = s_lin[e][2] = s_lab[e][0] * s_lab[e][0] * s_lab[e][0];
        s_neutrals[s_n_neutral++]               = static_cast<uint8_t>(e);
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
    if(!s_cache) s_cache = static_cast<target*>(heap_caps_malloc(CACHE_SLOTS * sizeof(target), MALLOC_CAP_SPIRAM));
    if(!s_cache)
    {
        ESP_LOGE(TAG, "no PSRAM for the target cache of the float dither");
        return false;
    }
    memset(s_cache, 0, CACHE_SLOTS * sizeof(target));
    return true;
}
} // namespace

int tones_float_ref_solid_gray(int g)
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

void tone_dither_float::alloc(int w_)
{
    w = w_;
    for(auto& r : rows) r.assign(static_cast<size_t>(w + 4) * 3, 0.0f);
}

void tone_dither_float::release()
{
    for(auto& r : rows) std::vector<float>().swap(r);
}

void tone_dither_float::row(const uint8_t* c, int y, uint8_t* out)
{
    // rp[k] receives the error for k rows down. Taps past either end of the row land in the 2 padding
    // cells, which are never read.
    float*        rp[3] = {rows[0].data(), rows[1].data(), rows[2].data()};
    float*        cur   = rp[0];
    const int8_t* noise = s_noise[y & (NOISE_N - 1)].data();
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
            // error is held inside that pair. A pixel on an entry takes it and leaves no error.
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
