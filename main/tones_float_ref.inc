// Floating-point reference for the fixed-point tone_dither in tones.cpp.
// Built only in the float-reference and benchmark builds.
// This fragment is included into tones.cpp and reads that file's tone tables.
// float_ref_init must run before any tone_dither_float::row or tones_float_ref_solid_gray call;
// tones_init runs it.

namespace
{
const float ERR_MAX = 0.25f;
const float ERR_ZERO = 1e-4f;
const float NEUTRAL_C = 1e-3f;
const float ON_ENTRY_L = 0.002f;
const int NB = 48;
const int CACHE_BITS = 10;
const float LUMA[3] = {0.2126f, 0.7152f, 0.0722f};

struct dither_tap
{
    int8_t dx, dy;
};
const dither_tap k_atkinson[6] = {{1, 0}, {2, 0}, {-1, 1}, {0, 1}, {1, 1}, {0, 2}};
const float      ATKINSON_W    = 1 / 8.0f;

const float    NOISE_REL_STEP = NOISE_REL / 64.0f;
const unsigned CACHE_SLOTS    = 1u << CACHE_BITS;

struct target
{
    uint32_t key;
    float    lin[3];
    uint8_t  lo, hi;
    bool     neutral;
};

float   s_lin[TONEMAP_COUNT][3];
uint8_t s_nb[TONEMAP_COUNT][NB];
uint8_t s_by_l[TONEMAP_COUNT];
uint8_t s_neutrals[TONEMAP_COUNT];
float   s_neutral_l[TONEMAP_COUNT];
int     s_n_neutral = 0;
float   s_byte_lin[256];
target* s_cache = nullptr;
#ifdef TONES_BENCH
int      s_bench_bits = CACHE_BITS;
uint32_t s_bench_miss = 0, s_bench_look = 0;
#endif

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

void oklab(uint8_t r8, uint8_t g8, uint8_t b8, float out[3])
{
    const float c[3] = {s_byte_lin[r8], s_byte_lin[g8], s_byte_lin[b8]};
    lin_to_oklab(c, out);
}

inline float dist(const float* a, const float* b)
{
    float dl = a[0] - b[0];
    float da = (a[1] - b[1]) * TONEMAP_CHROMA_WEIGHT;
    float db = (a[2] - b[2]) * TONEMAP_CHROMA_WEIGHT;
    return dl * dl + da * da + db * db;
}

inline float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

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

void bracket(float L, uint8_t* lo, uint8_t* hi)
{
    int k = 0;
    while(k < s_n_neutral - 1 && s_neutral_l[k] > L + ON_ENTRY_L) k++;
    *lo = s_neutrals[k];
    *hi = k == 0 || L - s_neutral_l[k] <= ON_ENTRY_L ? *lo : s_neutrals[k - 1];
}

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
}

int tones_float_ref_solid_gray(int g)
{
    g = std::max(0, std::min(255, g));
    target tg;
    make_target(static_cast<uint8_t>(g), static_cast<uint8_t>(g), static_cast<uint8_t>(g), &tg);
    const float l = cbrtf(tg.lin[0]);
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
        if(none && (r & g & b) == 255)
        {
            out[x] = TONE_PAPER;
            continue;
        }
        const target& t  = target_at(r, g, b);
        const int     nz = noise[x & (NOISE_N - 1)];
        unsigned      best;
        float         res[3];
        if(t.neutral)
        {
            if(t.lo == t.hi)
            {
                out[x] = t.lo;
                continue;
            }
            const float lo = s_lin[t.lo][0], hi = s_lin[t.hi][0];
            const float v  = clampf(t.lin[0] + LUMA[0] * ec[0] + LUMA[1] * ec[1] + LUMA[2] * ec[2], lo, hi);
            const float llo = s_lab[t.lo][0], lhi = s_lab[t.hi][0];
            const float m   = 0.5f * (llo + lhi - NOISE_REL_STEP * nz * (lhi - llo));
            best            = v > m * m * m ? t.hi : t.lo;
            res[0] = res[1] = res[2] = v - s_lin[best][0];
        }
        else
        {
            float v[3], p[3];
            for(int k = 0; k < 3; k++) v[k] = clampf(t.lin[k] + ec[k], 0.0f, 1.0f);
            lin_to_oklab(v, p);
            const uint8_t* cand = s_nb[t.lo];
            unsigned       next = cand[0];
            float          bd = 1e9f, nd = 1e9f;
            best                = cand[0];
            for(int k = 0; k < NB; k++)
            {
                unsigned f  = cand[k];
                float    dd = dist(p, s_lab[f]);
                if(dd < bd) nd = bd, next = best, bd = dd, best = f;
                else if(dd < nd) nd = dd, next = f;
            }
            if(nz && next != best)
            {
                unsigned lo = best, hi = next;
                float    dlo = bd, dhi = nd;
                if(s_lab[lo][0] > s_lab[hi][0]) std::swap(lo, hi), std::swap(dlo, dhi);
                best = dlo - dhi + NOISE_REL_STEP * nz * dist(s_lab[lo], s_lab[hi]) > 0 ? hi : lo;
            }
            for(int k = 0; k < 3; k++) res[k] = v[k] - s_lin[best][k];
        }
        out[x] = static_cast<uint8_t>(best);

        for(const dither_tap& tp : k_atkinson)
        {
            float* p = rp[tp.dy] + (x + tp.dx * d + 2) * 3;
            p[0] += res[0] * ATKINSON_W, p[1] += res[1] * ATKINSON_W, p[2] += res[2] * ATKINSON_W;
        }
    }
    std::swap(rows[0], rows[1]);
    std::swap(rows[1], rows[2]);
    std::fill(rows[2].begin(), rows[2].end(), 0.0f);
}
