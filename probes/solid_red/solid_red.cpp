// Host probe: which tone entries the fixed point dither (main/tones.cpp tone_dither) gives a solid
// sRGB fill. For each colour it dithers a 240 x 120 block and prints the share of each entry used.
// Usage: out/solid_red [R G B ...]. With no arguments it runs (v, 0, 0) for v from 200 to 255,
// then 226 57 22, the red reference entry's sRGB.
// out/solid_red -i ENTRY ... solves for each entry, given by index or pattern, the sRGB colour
// whose target through the float squeeze (main/tonemap_squeeze.h) is the entry's point on the
// squeeze's hue, or the deepest point that still takes the entry alone when that point does not,
// then dithers that colour as above.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tonemap.h"
#include "tonemap_squeeze.h"
#include "tones.h"

static const int W = 240, H = 120;

static float s_byte_lin[256];

// sRGB bytes to Oklab in float, the conversion that main/tonemap_squeeze.h is built on.
static void lin_oklab(const float c[3], float out[3])
{
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static void srgb_oklab(uint8_t r, uint8_t g, uint8_t b, float out[3])
{
    const float c[3] = {s_byte_lin[r], s_byte_lin[g], s_byte_lin[b]};
    lin_oklab(c, out);
}

static void oklab_lin(const float t[3], float c[3])
{
    const float l = t[0] + 0.3963377774f * t[1] + 0.2158037573f * t[2];
    const float m = t[0] - 0.1055613458f * t[1] - 0.0638541728f * t[2];
    const float s = t[0] - 0.0894841775f * t[1] - 1.2914855480f * t[2];
    const float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    c[0] = +4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
    c[1] = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
    c[2] = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;
}

// Linear light to the sRGB byte scale, unclamped.
static float lin_enc(float c)
{
    const float v = c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return v * 255.0f;
}

static int lin_byte(float c)
{
    const int b = static_cast<int>(lroundf(lin_enc(c)));
    return b < 0 ? 0 : b > 255 ? 255 : b;
}

// In gamut when every channel rounds to a byte without a clamp.
static bool in_gamut(const float c[3])
{
    for(int k = 0; k < 3; k++)
    {
        const float v = lin_enc(c[k]);
        if(v < -0.5f || v > 255.5f) return false;
    }
    return true;
}

// The Oklab colour on the squeeze's hue at L0 with chroma c.
static void on_hue(float L0, float c, float t[3])
{
    t[0] = L0;
    t[1] = c * s_squeeze_hue[0];
    t[2] = c * s_squeeze_hue[1];
}

static void probe(int r, int g, int b)
{
    tone_dither q;
    q.alloc(W);
    std::vector<uint8_t> rgb(W * 3), out(W);
    for(int x = 0; x < W; x++)
    {
        rgb[x * 3]     = static_cast<uint8_t>(r);
        rgb[x * 3 + 1] = static_cast<uint8_t>(g);
        rgb[x * 3 + 2] = static_cast<uint8_t>(b);
    }
    std::vector<int> n(TONEMAP_COUNT, 0);
    for(int y = 0; y < H; y++)
    {
        q.row(rgb.data(), y, out.data());
        for(int x = 0; x < W; x++) n[out[x]]++;
    }
    int used = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++) used += n[e] ? 1 : 0;
    printf("%3d %3d %3d  #%02x%02x%02x  %d entries:", r, g, b, r, g, b, used);
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(n[e])
            printf("  e%u %.1f%% (L %.4f a %+.4f b %+.4f %s)", e, 100.0 * n[e] / (W * H), k_tonemap[e].L,
                   k_tonemap[e].a, k_tonemap[e].b, k_tonemap[e].pattern);
    printf("\n");
}

static int entry_arg(const char* s)
{
    char* end;
    const long i = strtol(s, &end, 10);
    if(*s && !*end) return i >= 0 && i < static_cast<long>(TONEMAP_COUNT) ? static_cast<int>(i) : -1;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(strcmp(k_tonemap[e].pattern, s) == 0) return static_cast<int>(e);
    return -1;
}

// The search's distance, chroma weighted as in tone_fx_search.
static float dist(const float* p, const tone_entry& n)
{
    const float dl = p[0] - n.L, da = TONEMAP_CHROMA_WEIGHT * (p[1] - n.a), db = TONEMAP_CHROMA_WEIGHT * (p[2] - n.b);
    return dl * dl + da * da + db * db;
}

// How much nearer entry e is than entry n, for the target at L and chroma c on the hue (side 0) or
// for the point the search sees once the carried error settles (side 1). Atkinson carries 6/8 of
// each pixel's error in linear light, so with e chosen every time the carried error settles at
// 3 (T - E) and the search sees 4 T - 3 E.
static float lead(int e, int n, float L, float c, int side)
{
    float t[3], p[3];
    on_hue(L, c, t);
    if(side)
    {
        const float el[3] = {k_tonemap[e].L, k_tonemap[e].a, k_tonemap[e].b};
        float       tl[3], ell[3], pl[3];
        oklab_lin(t, tl);
        oklab_lin(el, ell);
        for(int k = 0; k < 3; k++) pl[k] = 4.0f * tl[k] - 3.0f * ell[k];
        lin_oklab(pl, p);
    }
    else
    {
        for(int k = 0; k < 3; k++) p[k] = t[k];
    }
    return dist(p, k_tonemap[n]) - dist(p, k_tonemap[e]);
}

// How far inside the solid region the target at L and chroma c on the hue lies, in Oklab units of
// the target. The region is where the target and its settled point both have e nearest, with the
// chroma inside the panel edge, so a fill there takes e alone. Each lead is linear in the target,
// or close to it for the settled point, which moves 4 times as far, so each lead over its slope is
// a distance and the margin is the least of them.
static float margin(int e, float L, float c)
{
    const float pan   = squeeze_pan_at(L);
    const float el[3] = {k_tonemap[e].L, k_tonemap[e].a, k_tonemap[e].b};
    float       m     = pan - c < c - 0.002f ? pan - c : c - 0.002f;
    for(int n = 0; n < static_cast<int>(TONEMAP_COUNT); n++)
    {
        if(n == e) continue;
        const float g = 2.0f * sqrtf(dist(el, k_tonemap[n]));
        const float t = lead(e, n, L, c, 0) / g, p = lead(e, n, L, c, 1) / (4.0f * g);
        m = t < m ? t : m;
        m = p < m ? p : m;
    }
    return m;
}

// The maximum of a function that is concave on [a, b], by golden section.
template <typename F> static float peak(F f, float a, float b)
{
    const float k = 0.381966f;
    float       x = a + k * (b - a), y = b - k * (b - a), fx = f(x), fy = f(y);
    for(int i = 0; i < 48; i++)
    {
        if(fx < fy) a = x, x = y, fx = fy, y = b - k * (b - a), fy = f(y);
        else b = y, y = x, fy = fx, x = a + k * (b - a), fx = f(x);
    }
    return 0.5f * (a + b);
}

// The target for entry e on the squeeze's hue: the entry's projection on the hue when that is
// solid, else the point deepest inside the solid region. The margin is the least of near linear
// functions, so it is concave and golden section over L, with golden section over the chroma
// inside it, finds the peak.
static bool target_of(int e, float cp, float* L, float* c)
{
    const float L0 = k_tonemap[e].L;
    *L = L0, *c = cp;
    const float mp = margin(e, L0, cp);
    if(mp > 0.0f)
    {
        printf("  the projection is solid, margin %.4f\n", mp);
        return true;
    }
    auto best_c = [e](float l) { return peak([e, l](float x) { return margin(e, l, x); }, 0.002f, squeeze_pan_at(l)); };
    const float lo = L0 - 0.15f > 0.01f ? L0 - 0.15f : 0.01f, hi = L0 + 0.15f < 0.99f ? L0 + 0.15f : 0.99f;
    *L = peak([e, &best_c](float l) { return margin(e, l, best_c(l)); }, lo, hi);
    *c = best_c(*L);
    const float m = margin(e, *L, *c);
    printf("  the projection is not solid, margin %.4f. Deepest solid point L %.4f C %.4f, margin %.4f\n", mp, *L,
           *c, m);
    return m > 0.0f;
}

// The squeeze's L2 depends on L0 alone and rises with it, so bisection on L0 meets the target's L.
// The chroma is then closed form: the target takes share * pan(L2) with share = c / edge(L0).
static void solve(int e)
{
    const tone_entry& en = k_tonemap[e];
    const float       cp = en.a * s_squeeze_hue[0] + en.b * s_squeeze_hue[1];
    const float       cx = -en.a * s_squeeze_hue[1] + en.b * s_squeeze_hue[0];
    printf("e%d %s  L %.4f  C on hue %.4f, off hue %+.4f\n", e, en.pattern, en.L, cp, cx);
    float lt, ct;
    if(!target_of(e, cp, &lt, &ct))
    {
        printf("  no target on the hue takes the entry alone\n");
        return;
    }
    float l0 = 0.0f, l1 = 1.0f;
    for(int i = 0; i < 40; i++)
    {
        const float mid  = 0.5f * (l0 + l1);
        float       t[3] = {mid, 0.0f, 0.0f};
        tonemap_squeeze(t);
        (t[0] < lt ? l0 : l1) = mid;
    }
    const float L0   = 0.5f * (l0 + l1);
    float       t[3] = {L0, 0.0f, 0.0f};
    tonemap_squeeze(t);
    const float L2 = t[0], pan = squeeze_pan_at(L2), edge = squeeze_at(s_squeeze_img, L0);
    printf("  L0 %.5f gives L2 %.5f, pan %.4f, edge %.4f\n", L0, L2, pan, edge);
    if(ct > pan)
    {
        printf("  the entry lies past the panel edge at its L, no preimage\n");
        return;
    }
    float c = ct / pan * edge, lab[3], lin[3];
    on_hue(L0, c, lab);
    oklab_lin(lab, lin);
    printf("  preimage Oklab %.5f %+.5f %+.5f, linear %.5f %.5f %.5f\n", lab[0], lab[1], lab[2], lin[0], lin[1], lin[2]);
    if(!in_gamut(lin))
    {
        // Toward gray at the same L0 to the gamut edge.
        float in = 0.0f, out = c;
        for(int i = 0; i < 40; i++)
        {
            const float mid = 0.5f * (in + out);
            on_hue(L0, mid, lab);
            oklab_lin(lab, lin);
            (in_gamut(lin) ? in : out) = mid;
        }
        const float cin = in / edge * pan, m = margin(e, L2, cin);
        printf("  outside sRGB, chroma %.5f steps toward gray to %.5f, target C %.4f, margin %.4f, %s\n", c, in,
               cin, m, m > 0.0f ? "still solid" : "not solid");
        c = in;
        on_hue(L0, c, lab);
        oklab_lin(lab, lin);
    }
    const int r = lin_byte(lin[0]), g = lin_byte(lin[1]), b = lin_byte(lin[2]);
    float     q[3];
    srgb_oklab(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), q);
    tonemap_squeeze(q);
    printf("  #%02x%02x%02x  float target of the bytes: L %.4f C %.4f, solved L %.4f C %.4f\n", r, g, b, q[0],
           sqrtf(q[1] * q[1] + q[2] * q[2]), lt, ct);
    probe(r, g, b);
}

int main(int argc, char** argv)
{
    if(!tones_init()) return 1;
    if(argc >= 2 && strcmp(argv[1], "-i") == 0)
    {
        for(int v = 0; v < 256; v++)
        {
            const float c = v / 255.0f;
            s_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
        }
        tonemap_squeeze_begin(srgb_oklab);
        for(int i = 2; i < argc; i++)
        {
            const int e = entry_arg(argv[i]);
            if(e < 0)
            {
                printf("%s: no such entry\n", argv[i]);
                return 1;
            }
            solve(e);
        }
        return 0;
    }
    if(argc >= 4)
    {
        for(int i = 1; i + 2 < argc; i += 3) probe(atoi(argv[i]), atoi(argv[i + 1]), atoi(argv[i + 2]));
        return 0;
    }
    for(int v = 200; v <= 255; v++) probe(v, 0, 0);
    probe(226, 57, 22);
    return 0;
}
