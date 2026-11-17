// The parameters of the squeeze kernel (main/tone_fx_squeeze.cpp), fitted in float to the edge
// tables of main/tonemap_squeeze.h. The kernel's file describes the curves and their parameters.
//
// In the generator's link this file stands in for main/tone_fx_squeeze.cpp: it defines
// tone_fx_squeeze_params(), so the stand in of the kernel reads the parameters built here.

#include <cmath>
#include <cstdint>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "tonemap_squeeze.h"
#pragma GCC diagnostic pop

#include "tone_fx_squeeze.h"
#include "tone_fx_squeeze_internal.h"
#include "tone_tables_build.h"

static_assert(TONEMAP_GAMMA == 1.0f, "the fixed point squeeze has no tone curve on L");
static_assert(TONEMAP_COMPRESS >= 0.0f && TONEMAP_COMPRESS <= 1.0f, "the fixed point squeeze always squeezes");

namespace
{
using namespace tone_fx_squeeze_internal;

// Lines of the panel edge. The kernel evaluates exactly this many.
const int PANEL_LINES = 4;

// The build fails when a fitted edge misses its table by more than this, in real units.
const float FIT_MAX = 0.0005f;

static_assert(PANEL_LINES == 4, "the kernels and the parameter order hold four panel lines");

alignas(16) int16_t s_k[K_COUNT];

// sRGB bytes to Oklab in float, the conversion of tones.cpp, for the edge tables.
float s_byte_lin[256];

void srgb_oklab(uint8_t r8, uint8_t g8, uint8_t b8, float out[3])
{
    const float c[3] = {s_byte_lin[r8], s_byte_lin[g8], s_byte_lin[b8]};
    const float l    = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m    = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s    = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]           = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]           = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]           = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

// A q15 parameter, false when v is outside -1 to 1.
bool param(float v, int idx)
{
    if(!(v > -1.0f && v < 1.0f)) return false;
    s_k[idx] = q15_from(v);
    return true;
}

// The upper branch of the image table over x = 1 - L from 0 to xr as x * (c[0] + c[1] x + c[2] x^2),
// near minimax. Each round is least squares reweighted by the error (Lawson), and the best round is
// kept. The fit runs in t = x / xr so the normal equations stay well scaled in float.
void fit_upper(float xr, float c[3])
{
    const int NS = 256;
    float     y[NS + 1], w[NS + 1];
    for(int i = 0; i <= NS; i++)
    {
        y[i] = squeeze_at(s_squeeze_img, 1.0f - xr * static_cast<float>(i) / NS);
        w[i] = 1.0f;
    }
    float best = 1e9f;
    for(int round = 0; round < 40; round++)
    {
        float M[3][4] = {};
        for(int i = 0; i <= NS; i++)
        {
            const float t     = static_cast<float>(i) / NS;
            const float ph[3] = {t, t * t, t * t * t};
            for(int r = 0; r < 3; r++)
            {
                for(int j = 0; j < 3; j++) M[r][j] += w[i] * ph[r] * ph[j];
                M[r][3] += w[i] * ph[r] * y[i];
            }
        }
        for(int r = 0; r < 3; r++)
            for(int r2 = r + 1; r2 < 3; r2++)
            {
                const float f = M[r2][r] / M[r][r];
                for(int j = r; j < 4; j++) M[r2][j] -= f * M[r][j];
            }
        float d[3];
        for(int r = 2; r >= 0; r--)
        {
            float s = M[r][3];
            for(int j = r + 1; j < 3; j++) s -= M[r][j] * d[j];
            d[r] = s / M[r][r];
        }
        float mx = 0.0f, sw = 0.0f;
        for(int i = 0; i <= NS; i++)
        {
            const float t  = static_cast<float>(i) / NS;
            const float er = fabsf(t * (d[0] + t * (d[1] + t * d[2])) - y[i]);
            mx             = fmaxf(mx, er);
            w[i] *= er + 1e-9f;
            sw += w[i];
        }
        for(int i = 0; i <= NS; i++) w[i] /= sw;
        if(mx < best)
        {
            best = mx;
            c[0] = d[0] / xr;
            c[1] = d[1] / (xr * xr);
            c[2] = d[2] / (xr * xr * xr);
        }
    }
}

struct pan_line
{
    float m, b;
};

// Largest amount by which the minimum of n lines misses the panel table, over 1024 steps of L.
float pan_miss(const pan_line* ln, int n)
{
    float mx = 0.0f;
    for(int i = 0; i <= 1024; i++)
    {
        const float L = static_cast<float>(i) / 1024.0f;
        float       v = 1e9f;
        for(int j = 0; j < n; j++) v = fminf(v, ln[j].m * L + ln[j].b);
        mx = fmaxf(mx, fabsf(v - squeeze_at(s_squeeze_pan, L)));
    }
    return mx;
}

// The panel table as PANEL_LINES lines, line 0 through black. False when that does not fit.
bool fit_panel(pan_line out[PANEL_LINES])
{
    pan_line ln[SQUEEZE_STEPS];
    int      n = 0;
    for(int i = 0; i < SQUEEZE_STEPS; i++)
    {
        const float l0 = static_cast<float>(i) / SQUEEZE_STEPS, l1 = static_cast<float>(i + 1) / SQUEEZE_STEPS;
        const float p0 = s_squeeze_pan[i], p1 = s_squeeze_pan[i + 1];
        // A step whose ends lie on the last line, within the float rounding of the table, is on it.
        if(n > 0 && fabsf(ln[n - 1].m * l0 + ln[n - 1].b - p0) < 2e-6f &&
           fabsf(ln[n - 1].m * l1 + ln[n - 1].b - p1) < 2e-6f)
            continue;
        const float m = (p1 - p0) * SQUEEZE_STEPS;
        ln[n++]       = {m, p0 - m * l0};
    }
    // Drop the line whose loss misses the table least until PANEL_LINES remain.
    while(n > PANEL_LINES)
    {
        int   drop = 0;
        float best = 1e9f;
        for(int j = 0; j < n; j++)
        {
            pan_line rest[SQUEEZE_STEPS];
            int      r = 0;
            for(int i = 0; i < n; i++)
                if(i != j) rest[r++] = ln[i];
            const float miss = pan_miss(rest, r);
            if(miss < best) best = miss, drop = j;
        }
        for(int i = drop; i + 1 < n; i++) ln[i] = ln[i + 1];
        n--;
    }
    // Line 0 is the one through black. The kernel adds no intercept to it.
    int z = 0;
    for(int j = 1; j < n; j++)
        if(fabsf(ln[j].b) < fabsf(ln[z].b)) z = j;
    if(fabsf(ln[z].b) > 1.0f / 32768.0f) return false;
    out[0] = ln[z];
    int o  = 1;
    for(int j = 0; j < n; j++)
        if(j != z) out[o++] = ln[j];
    // With fewer lines than slots the last one repeats, which leaves the minimum as it is.
    for(; o < PANEL_LINES; o++) out[o] = out[o - 1];
    return true;
}
} // namespace

bool tone_fx_squeeze_build(int16_t k[], float* err_img, float* err_pan)
{
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        s_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    tonemap_squeeze_begin(srgb_oklab);
    const float red_l = s_squeeze_red_l, red_c = s_squeeze_red_c;
    if(!(red_l > 0.0f && red_l < 1.0f && red_c > 0.0f)) return false;

    // Image edge. The table follows the ray up to the step before the one that holds red.
    const int ic = static_cast<int>(red_l * SQUEEZE_STEPS);
    if(ic < 1 || ic + 1 >= SQUEEZE_STEPS) return false;
    const float s = red_c / red_l;
    for(int i = 0; i <= ic; i++)
        if(fabsf(s_squeeze_img[i] - s * static_cast<float>(i) / SQUEEZE_STEPS) > 1e-5f) return false;
    const float l0 = static_cast<float>(ic) / SQUEEZE_STEPS, l1 = static_cast<float>(ic + 1) / SQUEEZE_STEPS;
    const float cm = (s_squeeze_img[ic + 1] - s_squeeze_img[ic]) * SQUEEZE_STEPS;
    float       u[3];
    fit_upper(1.0f - l1, u);

    pan_line pl[PANEL_LINES];
    if(!fit_panel(pl)) return false;

    int black = -1;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(strcmp(k_tonemap[e].pattern, TONEMAP_BLACK_PATTERN) == 0) black = static_cast<int>(e);
    if(black < 0) return false;

    const float g = TONEMAP_COMPRESS * (s_squeeze_cusp_l - red_l) / red_c;
    memset(s_k, 0, sizeof(s_k));
    bool ok = param(s_squeeze_hue[0], K_H0) && param(s_squeeze_hue[1], K_H1) && param(s, K_S) && param(l1, K_LC) &&
              param(cm, K_CM) && param(s_squeeze_img[ic] - cm * l0, K_CB) && param(1.0f - l0, K_XC) &&
              param(u[2], K_U3) && param(u[1], K_U2) && param(u[0], K_U1) && param(g, K_G) &&
              param(pl[0].m, K_PM0) && param(pl[1].m, K_PM1) && param(pl[1].b, K_PB1) && param(pl[2].m, K_PM2) &&
              param(pl[2].b, K_PB2) && param(pl[3].m, K_PM3) && param(pl[3].b, K_PB3) &&
              param(k_tonemap[black].L, K_LB);
    if(!ok) return false;
    s_k[K_ONE]  = Q15_ONE;
    s_k[K_LSB]  = 1;
    s_k[K_HALF] = 16384;
    s_k[K_H0B]  = s_k[K_H0];
    s_k[K_H1B]  = s_k[K_H1];

    // The kernel takes no floor on the panel edge, so it must be 0 or above for every L2 from 0 to
    // 1. Line 0 is, with a slope of 0 or above. Where the rounding takes the minimum below 0 on
    // another line, that line's intercept rises by 1, at most 4 times in all.
    if(s_k[K_PM0] < 0) return false;
    static const int pb[PANEL_LINES] = {-1, K_PB1, K_PB2, K_PB3}, pm[PANEL_LINES] = {K_PM0, K_PM1, K_PM2, K_PM3};
    int              raised          = 0;
    for(int v = 0; v <= Q15_ONE; v++)
    {
        const int16_t L = static_cast<int16_t>(v);
        while(pan_edge(s_k, L) < 0)
        {
            int j = 1;
            while(j < PANEL_LINES && adds(mul(L, s_k[pm[j]], 15), s_k[pb[j]]) != pan_edge(s_k, L)) j++;
            if(j == PANEL_LINES || ++raised > 4) return false;
            s_k[pb[j]]++;
        }
    }

    // Both edges in fixed point against their tables at every q15 lightness.
    float e_img = 0.0f, e_pan = 0.0f;
    for(int v = 0; v <= Q15_ONE; v++)
    {
        const int16_t L = static_cast<int16_t>(v);
        const float   x = q15_to(L);
        if(pan_edge(s_k, L) < 0) return false;
        e_img = fmaxf(e_img, fabsf(q15_to(img_edge(s_k, L)) - squeeze_at(s_squeeze_img, x)));
        e_pan = fmaxf(e_pan, fabsf(q15_to(pan_edge(s_k, L)) - squeeze_at(s_squeeze_pan, x)));
    }
    if(e_img > FIT_MAX || e_pan > FIT_MAX) return false;
    memcpy(k, s_k, sizeof(s_k));
    *err_img = e_img;
    *err_pan = e_pan;
    return true;
}

const int16_t* tone_fx_squeeze_params()
{
    return s_k;
}
