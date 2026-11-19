// Benchmark harness for the tone pipeline.
// Compiled only under TONES_BENCH and included into tones.cpp, from which it takes the pipeline
// functions, tables and shared state.
// It is not a standalone translation unit.
// Call tones_bench() from a task: it yields with vTaskDelay between and within stages.
// With TONES_BENCH_ALL set to 1 the per-stage kernel rows and the no-cache frame row also run.
// Otherwise tones_bench() still times the init calls, prints the heap lines, and runs the path and
// frame benchmarks.

#include <cstdio>

#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tone_fx_bracket.h"
#include "tone_fx_search.h"
#include "tone_fx_squeeze.h"
#include "tone_fx_srgb.h"

namespace
{
const int    REPS       = 10;
const int    FRAME_REPS = 3;
const int    YIELD_ROWS = 16;
const int    NP         = TONE_W;
const int    NC         = 64;
const int    NS[]       = {NC, NP};
const int    NS_DECODE[] = {NC, CHUNK, NP};
const int    BAND       = 64;
const size_t BAND_BYTES = static_cast<size_t>(TONE_W) * 3 * BAND;

static_assert(NC % 8 == 0 && CHUNK % 8 == 0 && NP % 8 == 0, "the kernels take multiples of 8 elements");
static_assert(BAND_BYTES % (3 * NC) == 0 && BAND_BYTES % (3 * CHUNK) == 0 && BAND_BYTES % (3 * NP) == 0,
              "a walk of the band ends on a call");

#ifndef TONES_BENCH_ALL
#define TONES_BENCH_ALL 0
#endif
const bool BENCH_ALL = TONES_BENCH_ALL;

template <typename F> uint32_t best_of(F body)
{
    uint32_t best = UINT32_MAX;
    for(int r = 0; r < REPS; r++)
    {
        const uint32_t c0 = esp_cpu_get_cycle_count();
        body();
        const uint32_t dt = esp_cpu_get_cycle_count() - c0;
        if(dt < best) best = dt;
    }
    return best;
}

template <typename Fn, typename... A> uint32_t time_call(Fn fn, A... a)
{
    Fn volatile f = fn;
    return best_of([&] { f(a...); });
}

template <typename... A> void empty(A...) {}

void row(const char* stage, const char* form, int n, const char* where, uint32_t cycles, int per)
{
    printf("BENCH %-15s %-13s %6d %-8s %10.2f\n", stage, form, n, where,
           static_cast<double>(cycles) / static_cast<double>(per));
}

void heap_line(const char* when)
{
    printf("BENCH heap %-10s free internal %u, largest internal block %u, free PSRAM %u\n", when,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

constexpr uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

struct bench_mem
{
    uint8_t* rgb;
    uint8_t* band;
    float*   fin[3];
    q15*     qin[3];
    uint8_t* near;
    float*   fout[7];
    q15*     qout[7];
    uint8_t* lo;
    uint8_t* hi;
    float*   f1in;
    float*   f1out;
    q15*     q1in;
    q15*     q1out;
};

bench_mem s_m;
void*     s_alloc[40];
int       s_n_alloc = 0;

template <typename T> T* take(size_t count, uint32_t caps)
{
    void* p = heap_caps_aligned_alloc(16, count * sizeof(T), caps);
    if(p) memset(p, 0, count * sizeof(T));
    if(s_n_alloc < static_cast<int>(sizeof(s_alloc) / sizeof(s_alloc[0]))) s_alloc[s_n_alloc++] = p;
    return static_cast<T*>(p);
}

bool alloc_all()
{
    const uint32_t in = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_m.rgb           = take<uint8_t>(3 * NP + 16, in);
    s_m.band          = take<uint8_t>(BAND_BYTES + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for(int c = 0; c < 3; c++) s_m.fin[c] = take<float>(NP, in), s_m.qin[c] = take<q15>(NP, in);
    s_m.near = take<uint8_t>(NP, in);
    for(int c = 0; c < 7; c++) s_m.fout[c] = take<float>(NP, in), s_m.qout[c] = take<q15>(NP, in);
    s_m.lo    = take<uint8_t>(NP, in);
    s_m.hi    = take<uint8_t>(NP, in);
    s_m.f1in  = take<float>(4 * NC, in);
    s_m.f1out = take<float>(4 * NC, in);
    s_m.q1in  = take<q15>(8 * NC, in);
    s_m.q1out = take<q15>(8 * NC, in);
    bool ok   = s_n_alloc < static_cast<int>(sizeof(s_alloc) / sizeof(s_alloc[0]));
    for(int i = 0; i < s_n_alloc; i++) ok = ok && s_alloc[i];
    return ok;
}

void free_all()
{
    for(int i = 0; i < s_n_alloc; i++) heap_caps_free(s_alloc[i]);
    s_n_alloc = 0;
    s_m       = bench_mem();
}

void gen_pixels()
{
    for(int i = 0; i < NP; i++)
    {
        uint8_t*       p = s_m.rgb + 3 * i;
        const uint32_t h = hash32(static_cast<uint32_t>(i) * 2654435761u + 17u);
        p[0] = static_cast<uint8_t>(h), p[1] = static_cast<uint8_t>(h >> 8), p[2] = static_cast<uint8_t>(h >> 16);
        if(i & 1) p[1] = p[2] = static_cast<uint8_t>(p[1] / 2);
    }
    for(size_t i = 0; i < BAND_BYTES; i++) s_m.band[i] = static_cast<uint8_t>(hash32(static_cast<uint32_t>(i)) >> 24);
}

enum in_kind
{
    IN_LIN,
    IN_LAB,
    IN_SQUEEZED,
};

void gen_inputs(in_kind k)
{
    const float black_l = s_lab[s_black][0];
    for(int i = 0; i < NP; i++)
    {
        const uint8_t* p    = s_m.rgb + 3 * i;
        float          v[3] = {s_byte_lin[p[0]], s_byte_lin[p[1]], s_byte_lin[p[2]]};
        if(k != IN_LIN)
        {
            float lab[3];
            lin_to_oklab(v, lab);
            if(k == IN_SQUEEZED)
            {
                tonemap_squeeze(lab);
                lab[0]     = std::max(lab[0], black_l);
                s_m.near[i] = static_cast<uint8_t>(nearest(lab));
            }
            memcpy(v, lab, sizeof(v));
        }
        for(int c = 0; c < 3; c++) s_m.fin[c][i] = v[c], s_m.qin[c][i] = q15_from(v[c]);
    }
}

void f_decode(const uint8_t* rgb, int n, float* r, float* g, float* b)
{
    for(int i = 0; i < n; i++, rgb += 3) r[i] = s_byte_lin[rgb[0]], g[i] = s_byte_lin[rgb[1]], b[i] = s_byte_lin[rgb[2]];
}

void f_cbrt(const float* x, int n, float* y)
{
    for(int i = 0; i < n; i++) y[i] = cbrtf(x[i]);
}

void f_lin_to_oklab(const float* r, const float* g, const float* b, int n, float* L, float* A, float* B)
{
    for(int i = 0; i < n; i++)
    {
        const float c[3] = {r[i], g[i], b[i]};
        float       o[3];
        lin_to_oklab(c, o);
        L[i] = o[0], A[i] = o[1], B[i] = o[2];
    }
}

void f_oklab_to_lin(const float* L, const float* A, const float* B, int n, float* r, float* g, float* b)
{
    for(int i = 0; i < n; i++)
    {
        const float lab[3] = {L[i], A[i], B[i]};
        float       o[3];
        oklab_to_lin(lab, o);
        r[i] = clampf(o[0], 0.0f, 1.0f), g[i] = clampf(o[1], 0.0f, 1.0f), b[i] = clampf(o[2], 0.0f, 1.0f);
    }
}

void f_lin_to_oklab_1(const float* rgb, float* lab)
{
    lin_to_oklab(rgb, lab);
}

void f_squeeze(const float* L, const float* A, const float* B, int n, float* Lo, float* Ao, float* Bo, float* Co)
{
    const float black_l = s_lab[s_black][0];
    for(int i = 0; i < n; i++)
    {
        float t[3] = {L[i], A[i], B[i]};
        tonemap_squeeze(t);
        Lo[i] = std::max(t[0], black_l), Ao[i] = t[1], Bo[i] = t[2];
        Co[i] = t[1] * s_squeeze_hue[0] + t[2] * s_squeeze_hue[1];
    }
}

void f_bracket(const float* L, int n, uint8_t* lo, uint8_t* hi)
{
    for(int i = 0; i < n; i++) bracket(L[i], lo + i, hi + i);
}

void f_search_all(float L, float A, float B, uint8_t* best, uint8_t* next)
{
    const float p[3] = {L, A, B};
    unsigned    b = 0, x = 0;
    float       bd = 1e9f, nd = 1e9f;
    for(unsigned f = 0; f < N; f++)
    {
        const float dd = dist(p, s_lab[f]);
        if(dd < bd) nd = bd, x = b, bd = dd, b = f;
        else if(dd < nd) nd = dd, x = f;
    }
    *best = static_cast<uint8_t>(b), *next = static_cast<uint8_t>(x);
}

void f_search_nb(float L, float A, float B, uint8_t near, uint8_t* best, uint8_t* next)
{
    const float    p[3] = {L, A, B};
    const uint8_t* cand = s_nb[near];
    unsigned       b = cand[0], x = cand[0];
    float          bd = 1e9f, nd = 1e9f;
    for(int k = 0; k < NB; k++)
    {
        const unsigned f  = cand[k];
        const float    dd = dist(p, s_lab[f]);
        if(dd < bd) nd = bd, x = b, bd = dd, b = f;
        else if(dd < nd) nd = dd, x = f;
    }
    *best = static_cast<uint8_t>(b), *next = static_cast<uint8_t>(x);
}

struct chain_f
{
    float*   p[7];
    uint8_t* lo;
    uint8_t* hi;
};
struct chain_q
{
    q15*     p[7];
    uint8_t* lo;
    uint8_t* hi;
};

void pass_a_float(const uint8_t* rgb, int n, const chain_f* o)
{
    const float black_l = s_lab[s_black][0];
    for(int i = 0; i < n; i++, rgb += 3)
    {
        float lab[3], lin[3];
        oklab(rgb[0], rgb[1], rgb[2], lab);
        tonemap_squeeze(lab);
        lab[0] = std::max(lab[0], black_l);
        oklab_to_lin(lab, lin);
        o->p[0][i] = lab[0], o->p[1][i] = lab[1], o->p[2][i] = lab[2];
        o->p[3][i] = lab[1] * s_squeeze_hue[0] + lab[2] * s_squeeze_hue[1];
        for(int k = 0; k < 3; k++) o->p[4 + k][i] = clampf(lin[k], 0.0f, 1.0f);
        bracket(lab[0], o->lo + i, o->hi + i);
    }
}

void pass_a_pie(const uint8_t* rgb, int n, const chain_q* o)
{
    q15* const* p = o->p;
    tone_fx_srgb_to_lin(rgb, n, p[4], p[5], p[6]);
    tone_fx_lin_to_oklab(p[4], p[5], p[6], n, p[0], p[1], p[2]);
    tone_fx_squeeze(p[0], p[1], p[2], n, p[0], p[1], p[2], p[3]);
    tone_fx_oklab_to_lin(p[0], p[1], p[2], n, p[4], p[5], p[6]);
    tone_fx_bracket(p[0], n, o->lo, o->hi);
}

void stage_decode()
{
    const uint8_t* rgb = s_m.rgb;
    float* const*  fo  = s_m.fout;
    q15* const*    qo  = s_m.qout;
    for(int n : NS_DECODE)
    {
        row("srgb_decode", "float", n, "internal", time_call(f_decode, rgb, n, fo[0], fo[1], fo[2]), n);
        row("srgb_decode", "pie", n, "internal", time_call(tone_fx_srgb_to_lin, rgb, n, qo[0], qo[1], qo[2]), n);
        row("srgb_decode", "empty_float", n, "internal",
            time_call(empty<const uint8_t*, int, float*, float*, float*>, rgb, n, fo[0], fo[1], fo[2]), n);
        row("srgb_decode", "empty_pie", n, "internal",
            time_call(empty<const uint8_t*, int, q15*, q15*, q15*>, rgb, n, qo[0], qo[1], qo[2]), n);
    }
}

template <typename Fn, typename T> uint32_t time_walk(Fn fn, int n, T* r, T* g, T* b)
{
    Fn volatile    f     = fn;
    const int      calls = static_cast<int>(BAND_BYTES / (3 * static_cast<size_t>(n)));
    const uint8_t* band  = s_m.band;
    return best_of([&] {
        for(int k = 0; k < calls; k++) f(band + static_cast<size_t>(3 * n) * k, n, r, g, b);
    });
}

void stage_decode_psram()
{
    const int     px = static_cast<int>(BAND_BYTES / 3);
    float* const* fo = s_m.fout;
    q15* const*   qo = s_m.qout;
    for(int n : NS_DECODE)
    {
        row("srgb_decode", "float", n, "psram", time_walk(f_decode, n, fo[0], fo[1], fo[2]), px);
        row("srgb_decode", "pie", n, "psram", time_walk(tone_fx_srgb_to_lin, n, qo[0], qo[1], qo[2]), px);
        row("srgb_decode", "empty_float", n, "psram",
            time_walk(empty<const uint8_t*, int, float*, float*, float*>, n, fo[0], fo[1], fo[2]), px);
        row("srgb_decode", "empty_pie", n, "psram",
            time_walk(empty<const uint8_t*, int, q15*, q15*, q15*>, n, qo[0], qo[1], qo[2]), px);
    }
}

void stage_cbrt()
{
    gen_inputs(IN_LIN);
    const float* fx = s_m.fin[1];
    const q15*   qx = s_m.qin[1];
    for(int n : NS)
    {
        row("cbrt", "float", n, "internal", time_call(f_cbrt, fx, n, s_m.fout[0]), n);
        row("cbrt", "pie", n, "internal", time_call(tone_fx_cbrt, qx, n, s_m.qout[0]), n);
        row("cbrt", "empty_float", n, "internal", time_call(empty<const float*, int, float*>, fx, n, s_m.fout[0]), n);
        row("cbrt", "empty_pie", n, "internal", time_call(empty<const q15*, int, q15*>, qx, n, s_m.qout[0]), n);
    }
}

template <typename FF, typename FP> void stage_3to3(const char* stage, in_kind kind, FF ff, FP fp)
{
    gen_inputs(kind);
    const float* const* fi = s_m.fin;
    const q15* const*   qi = s_m.qin;
    float* const*       fo = s_m.fout;
    q15* const*         qo = s_m.qout;
    for(int n : NS)
    {
        row(stage, "float", n, "internal", time_call(ff, fi[0], fi[1], fi[2], n, fo[0], fo[1], fo[2]), n);
        row(stage, "pie", n, "internal", time_call(fp, qi[0], qi[1], qi[2], n, qo[0], qo[1], qo[2]), n);
        row(stage, "empty_float", n, "internal",
            time_call(empty<const float*, const float*, const float*, int, float*, float*, float*>, fi[0], fi[1],
                      fi[2], n, fo[0], fo[1], fo[2]),
            n);
        row(stage, "empty_pie", n, "internal",
            time_call(empty<const q15*, const q15*, const q15*, int, q15*, q15*, q15*>, qi[0], qi[1], qi[2], n,
                      qo[0], qo[1], qo[2]),
            n);
    }
}

template <typename Fn, typename T> uint32_t time_one(Fn fn, const T* in, T* out, int stride, int n)
{
    Fn volatile f = fn;
    return best_of([&] {
        for(int k = 0; k < n; k++) f(in + stride * k, out + stride * k);
    });
}

void stage_lin_to_oklab_1()
{
    gen_inputs(IN_LIN);
    for(int k = 0; k < NC; k++)
        for(int c = 0; c < 3; c++) s_m.f1in[4 * k + c] = s_m.fin[c][k], s_m.q1in[8 * k + c] = s_m.qin[c][k];
    const char* st = "lin_to_oklab_1";
    row(st, "float", NC, "internal", time_one(f_lin_to_oklab_1, s_m.f1in, s_m.f1out, 4, NC), NC);
    row(st, "pie", NC, "internal", time_one(tone_fx_lin_to_oklab_1, s_m.q1in, s_m.q1out, 8, NC), NC);
    row(st, "empty_float", NC, "internal", time_one(empty<const float*, float*>, s_m.f1in, s_m.f1out, 4, NC), NC);
    row(st, "empty_pie", NC, "internal", time_one(empty<const q15*, q15*>, s_m.q1in, s_m.q1out, 8, NC), NC);
}

void stage_squeeze()
{
    gen_inputs(IN_LAB);
    const float* const* fi = s_m.fin;
    const q15* const*   qi = s_m.qin;
    float* const*       fo = s_m.fout;
    q15* const*         qo = s_m.qout;
    for(int n : NS)
    {
        row("squeeze", "float", n, "internal",
            time_call(f_squeeze, fi[0], fi[1], fi[2], n, fo[0], fo[1], fo[2], fo[3]), n);
        row("squeeze", "pie", n, "internal",
            time_call(tone_fx_squeeze, qi[0], qi[1], qi[2], n, qo[0], qo[1], qo[2], qo[3]), n);
        row("squeeze", "empty_float", n, "internal",
            time_call(empty<const float*, const float*, const float*, int, float*, float*, float*, float*>, fi[0],
                      fi[1], fi[2], n, fo[0], fo[1], fo[2], fo[3]),
            n);
        row("squeeze", "empty_pie", n, "internal",
            time_call(empty<const q15*, const q15*, const q15*, int, q15*, q15*, q15*, q15*>, qi[0], qi[1], qi[2], n,
                      qo[0], qo[1], qo[2], qo[3]),
            n);
    }
}

void stage_bracket()
{
    gen_inputs(IN_SQUEEZED);
    const float* fl = s_m.fin[0];
    const q15*   ql = s_m.qin[0];
    for(int n : NS)
    {
        row("bracket", "float", n, "internal", time_call(f_bracket, fl, n, s_m.lo, s_m.hi), n);
        row("bracket", "pie", n, "internal", time_call(tone_fx_bracket, ql, n, s_m.lo, s_m.hi), n);
        row("bracket", "empty_float", n, "internal",
            time_call(empty<const float*, int, uint8_t*, uint8_t*>, fl, n, s_m.lo, s_m.hi), n);
        row("bracket", "empty_pie", n, "internal",
            time_call(empty<const q15*, int, uint8_t*, uint8_t*>, ql, n, s_m.lo, s_m.hi), n);
    }
}

template <typename Fn, typename P> uint32_t time_search(Fn fn, const P* const* pt, int n)
{
    Fn volatile f = fn;
    const P *   L = pt[0], *A = pt[1], *B = pt[2];
    uint8_t *   best = s_m.lo, *next = s_m.hi;
    return best_of([&] {
        for(int k = 0; k < n; k++) f(L[k], A[k], B[k], best + k, next + k);
    });
}

template <typename Fn> uint32_t time_search_nb(Fn fn, int n)
{
    Fn volatile    f = fn;
    const float *  L = s_m.fin[0], *A = s_m.fin[1], *B = s_m.fin[2];
    const uint8_t* near = s_m.near;
    uint8_t *      best = s_m.lo, *next = s_m.hi;
    return best_of([&] {
        for(int k = 0; k < n; k++) f(L[k], A[k], B[k], near[k], best + k, next + k);
    });
}

void stage_search()
{
    gen_inputs(IN_SQUEEZED);
    const float* const* fp = s_m.fin;
    const q15* const*   qp = s_m.qin;
    const int           n  = NP;
    row("search", "float_all", n, "internal", time_search(f_search_all, fp, n), n);
    row("search", "float_nb48", n, "internal", time_search_nb(f_search_nb, n), n);
    row("search", "pie", n, "internal", time_search(tone_fx_search, qp, n), n);
    row("search", "empty_float", n, "internal", time_search(empty<float, float, float, uint8_t*, uint8_t*>, fp, n), n);
    row("search", "empty_nb", n, "internal", time_search_nb(empty<float, float, float, uint8_t, uint8_t*, uint8_t*>, n),
        n);
    row("search", "empty_pie", n, "internal", time_search(empty<q15, q15, q15, uint8_t*, uint8_t*>, qp, n), n);
}

void stage_pass_a()
{
    chain_f cf;
    chain_q cq;
    for(int c = 0; c < 7; c++) cf.p[c] = s_m.fout[c], cq.p[c] = s_m.qout[c];
    cf.lo = cq.lo = s_m.lo;
    cf.hi = cq.hi = s_m.hi;
    for(int n : NS)
    {
        row("pass_a", "float", n, "internal", time_call(pass_a_float, s_m.rgb, n, &cf), n);
        row("pass_a", "pie", n, "internal", time_call(pass_a_pie, s_m.rgb, n, &cq), n);
        row("pass_a", "empty_float", n, "internal",
            time_call(empty<const uint8_t*, int, const chain_f*>, s_m.rgb, n, &cf), n);
        row("pass_a", "empty_pie", n, "internal",
            time_call(empty<const uint8_t*, int, const chain_q*>, s_m.rgb, n, &cq), n);
    }
}

uint8_t empty_code(const tone_plan&, unsigned, tone_phase, unsigned)
{
    return 0;
}

void stage_plan_code()
{
    uint32_t count[TONEMAP_COUNT];
    for(uint32_t& c : count) c = 1;
    static tone_plan plan;
    tone_plan_build(count, &plan);
    const unsigned len = plan.len[TONE_PHASE_2];
    if(len == 0)
    {
        printf("BENCH tone_plan_code skipped: the plan has no phase 2\n");
        return;
    }
    typedef uint8_t (*code_fn)(const tone_plan&, unsigned, tone_phase, unsigned);
    auto time_code = [&](code_fn fn) {
        code_fn volatile f   = fn;
        uint8_t*         out = s_m.lo;
        return best_of([&] {
            for(unsigned k = 0; k < static_cast<unsigned>(NP); k++) out[k] = f(plan, k % N, TONE_PHASE_2, k % len);
        });
    };
    row("tone_plan_code", "call", NP, "internal", time_code(tone_plan_code), NP);
    row("tone_plan_code", "empty", NP, "internal", time_code(empty_code), NP);
}

enum
{
    IMG_TEXT,
    IMG_FLAT,
    IMG_PHOTO,
    IMG_NOISE,
    IMG_N
};
const char* const k_img[IMG_N] = {"frame_text", "frame_gradient", "frame_photo", "frame_noise"};

void gen_row(int img, int y, uint8_t* rgb)
{
    for(int x = 0; x < TONE_W; x++)
    {
        uint8_t*       p = rgb + 3 * x;
        const uint32_t h = hash32(static_cast<uint32_t>(x) * 7919u + static_cast<uint32_t>(y) * 104729u);
        if(img == IMG_TEXT)
        {
            p[0] = p[1] = p[2] = 255;
            if(y < 40) p[0] = p[1] = p[2] = 200;
            else if((y - 40) % 24 < 14)
            {
                const uint32_t g    = hash32(static_cast<uint32_t>(x / 3) + 4099u * static_cast<uint32_t>(y / 2));
                const bool     link = ((y - 40) / 24) % 7 == 3;
                if(g % 4 == 0)
                {
                    p[0] = p[1] = 0;
                    p[2]        = link ? 238 : 0;
                }
                else if(g % 8 == 1) p[0] = p[1] = p[2] = static_cast<uint8_t>(64 + (g >> 8) % 160);
            }
        }
        else if(img == IMG_NOISE)
        {
            p[0] = static_cast<uint8_t>(h), p[1] = static_cast<uint8_t>(h >> 8), p[2] = static_cast<uint8_t>(h >> 16);
        }
        else
        {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            float       c[3] = {128.0f + 100.0f * sinf(fx / 90.0f + fy / 200.0f),
                                128.0f + 100.0f * sinf(fx / 130.0f - fy / 70.0f + 1.0f),
                                128.0f + 90.0f * sinf((fx + fy) / 160.0f + 2.0f)};
            for(int k = 0; k < 3; k++)
            {
                if(img == IMG_PHOTO) c[k] += static_cast<float>((h >> (8 * k)) % 7) - 3.0f;
                p[k] = static_cast<uint8_t>(clampf(c[k], 0.0f, 255.0f));
            }
        }
    }
}

template <typename F> uint64_t time_frame(int img, uint8_t* band, uint8_t* dst, F dither)
{
    uint64_t cycles = 0;
    for(int y0 = 0; y0 < TONE_H; y0 += BAND)
    {
        const int n = std::min(BAND, TONE_H - y0);
        for(int y = 0; y < n; y++) gen_row(img, y0 + y, band + static_cast<size_t>(y) * TONE_W * 3);
        for(int ya = 0; ya < n; ya += YIELD_ROWS)
        {
            const int      yb = std::min(n, ya + YIELD_ROWS);
            const uint32_t c0 = esp_cpu_get_cycle_count();
            for(int y = ya; y < yb; y++)
                dither(y0 + y, band + static_cast<size_t>(y) * TONE_W * 3, dst + static_cast<size_t>(y0 + y) * TONE_W);
            cycles += static_cast<uint32_t>(esp_cpu_get_cycle_count() - c0);
            vTaskDelay(1);
        }
    }
    return cycles;
}

void run_frame(int img, const char* form, uint8_t* band)
{
    tone_dither_float q;
    q.alloc(TONE_W);
    if(s_bench_bits) memset(s_cache, 0, (static_cast<size_t>(1) << s_bench_bits) * sizeof(target));
    s_bench_miss = s_bench_look = 0;
    const uint64_t cycles = time_frame(img, band, s_frame, [&](int y, const uint8_t* rgb, uint8_t* out) { q.row(rgb, y, out); });
    q.release();
    const int px = TONE_W * TONE_H;
    printf("BENCH %-15s %-13s %6d %-8s %10.2f %7u lookups %7u misses\n", k_img[img], form, px, "psram",
           static_cast<double>(cycles) / px, static_cast<unsigned>(s_bench_look), static_cast<unsigned>(s_bench_miss));
}

struct split
{
    uint64_t     total = UINT64_MAX;
    uint32_t     cyc[PH_N];
    bench_counts n;
};

void keep_best(split& sp, uint64_t total)
{
    if(total >= sp.total) return;
    sp.total = total;
    memcpy(sp.cyc, s_bench_cyc, sizeof(sp.cyc));
}

void print_split(const char* stage, const split& sp, int px)
{
    double sum = 0;
    for(uint32_t v : sp.cyc) sum += v;
    const double p = px;
    printf("BENCH %-15s split decode %.2f compact %.2f kernels %.2f pass_b %.2f rest %.2f cycles/px\n", stage,
           sp.cyc[PH_DECODE] / p, sp.cyc[PH_COMPACT] / p, sp.cyc[PH_KERNELS] / p, sp.cyc[PH_PASS_B] / p,
           (static_cast<double>(sp.total) - sum) / p);
    const bench_counts& n = sp.n;
    printf("BENCH %-15s chunks %u white %u kernel %u (colour %u neutral %u) lanes %u\n", stage,
           static_cast<unsigned>(n.chunks), static_cast<unsigned>(n.white_chunks),
           static_cast<unsigned>(n.kernel_chunks), static_cast<unsigned>(n.colour_chunks),
           static_cast<unsigned>(n.neutral_chunks), static_cast<unsigned>(n.lanes));
    printf("BENCH %-15s px white_chunk %u entry_run %u (white %u) entry_carry %u neutral %u colour %u "
           "(runs %u) photo %u\n",
           stage, static_cast<unsigned>(n.white_chunk_px), static_cast<unsigned>(n.entry_run_px),
           static_cast<unsigned>(n.white_run_px), static_cast<unsigned>(n.entry_carry_px),
           static_cast<unsigned>(n.neutral_px), static_cast<unsigned>(n.colour_px),
           static_cast<unsigned>(n.colour_runs), static_cast<unsigned>(n.photo_px));
}

void run_frame_fixed(int img, uint8_t* band)
{
    split sp;
    auto  frame = [&] {
        tone_dither q;
        q.alloc(TONE_W);
        return time_frame(img, band, s_frame, [&](int y, const uint8_t* rgb, uint8_t* out) { q.row(rgb, y, out); });
    };
    for(int r = 0; r < FRAME_REPS; r++)
    {
        memset(s_bench_cyc, 0, sizeof(s_bench_cyc));
        keep_best(sp, frame());
    }
    s_bench_n     = bench_counts();
    s_bench_count = true;
    frame();
    s_bench_count = false;
    sp.n          = s_bench_n;
    const int px  = TONE_W * TONE_H;
    printf("BENCH %-15s %-13s %6d %-8s %10.2f %7u lanes   %7u colour\n", k_img[img], "pie", px, "psram",
           static_cast<double>(sp.total) / px, static_cast<unsigned>(sp.n.lanes),
           static_cast<unsigned>(sp.n.colour_px));
    print_split(k_img[img], sp, px);
}

enum
{
    PATH_WHITE_CHUNK,
    PATH_WHITE_RUN,
    PATH_BLACK,
    PATH_GRAY,
    PATH_GRAY_WW,
    PATH_COLOUR,
    PATH_COLOUR_RND,
    PATH_COLOUR_GRAY,
    PATH_N
};
const char* const k_path[PATH_N] = {"path_white_chk", "path_white_run", "path_black",      "path_gray",
                                    "path_gray_ww",   "path_colour",    "path_colour_rnd", "path_colour_gray"};
const int         PATH_ROWS      = 32;

void gen_path_row(int kind, uint8_t* rgb)
{
    for(int x = 0; x < TONE_W; x++)
    {
        uint8_t*       p = rgb + 3 * x;
        const uint32_t h = hash32(static_cast<uint32_t>(x) * 2654435761u + 7u);
        uint8_t        v = 255;
        if(kind == PATH_WHITE_RUN && x % CHUNK == 0) v = 0;
        if(kind == PATH_BLACK) v = 0;
        if(kind == PATH_GRAY || (kind == PATH_GRAY_WW && x % 3 == 0) || kind == PATH_COLOUR_GRAY) v = 128;
        p[0] = p[1] = p[2] = v;
        if(kind == PATH_COLOUR || (kind == PATH_COLOUR_GRAY && x % 2 == 0)) p[0] = 220, p[1] = 60, p[2] = 60;
        if(kind == PATH_COLOUR_RND)
        {
            const int r = 160 + static_cast<int>(h % 96);
            p[0]        = static_cast<uint8_t>(r);
            p[1]        = static_cast<uint8_t>((h >> 8) % static_cast<uint32_t>(r / 3));
            p[2]        = static_cast<uint8_t>((h >> 16) % static_cast<uint32_t>(r / 3));
        }
    }
}

void run_path(int kind)
{
    gen_path_row(kind, s_m.rgb);
    split sp;
    auto  rows = [&] {
        tone_dither q;
        q.alloc(TONE_W);
        const uint32_t c0 = esp_cpu_get_cycle_count();
        for(int y = 0; y < PATH_ROWS; y++) q.row(s_m.rgb, y, s_frame + static_cast<size_t>(y) * TONE_W);
        return static_cast<uint64_t>(esp_cpu_get_cycle_count() - c0);
    };
    for(int r = 0; r < FRAME_REPS; r++)
    {
        memset(s_bench_cyc, 0, sizeof(s_bench_cyc));
        keep_best(sp, rows());
    }
    s_bench_n     = bench_counts();
    s_bench_count = true;
    rows();
    s_bench_count = false;
    sp.n          = s_bench_n;
    const int px  = TONE_W * PATH_ROWS;
    row(k_path[kind], "pie", px, "internal", static_cast<uint32_t>(sp.total), px);
    print_split(k_path[kind], sp, px);
}

void stage_paths()
{
    for(int k = 0; k < PATH_N; k++)
    {
        run_path(k);
        vTaskDelay(1);
    }
}

void stage_frames()
{
    char cached[16];
    snprintf(cached, sizeof(cached), "float_c%u", CACHE_SLOTS);
    for(int img = 0; img < IMG_N; img++)
    {
        s_bench_bits = CACHE_BITS;
        run_frame(img, cached, s_m.band);
        if(BENCH_ALL)
        {
            s_bench_bits = 0;
            run_frame(img, "float_nocache", s_m.band);
        }
        run_frame_fixed(img, s_m.band);
    }
    s_bench_bits = CACHE_BITS;
    memset(s_cache, 0, CACHE_SLOTS * sizeof(target));
}

uint32_t s_c0;

void init_start()
{
    s_c0 = esp_cpu_get_cycle_count();
}

void init_row(const char* name, bool ok)
{
    const uint32_t dt = esp_cpu_get_cycle_count() - s_c0;
    printf("BENCH init %-30s %-6s %10u cycles\n", name, ok ? "ok" : "FAILED", static_cast<unsigned>(dt));
}
}

void tones_bench()
{
    heap_line("start");

    {
        static char why[160];
        init_start();
        init_row("tones_init, all of it", tones_init());
        init_start();
        const bool ok = tone_plan_init(why, sizeof(why));
        init_row("tone_plan_init", ok);
        init_start();
        init_row("float_ref_init", float_ref_init());
    }
    vTaskDelay(1);

    if(!alloc_all())
    {
        printf("BENCH rows skipped: no memory\n");
        free_all();
        heap_line("freed");
        printf("BENCH DONE\n");
        return;
    }
    heap_line("allocated");
    gen_pixels();
    printf("BENCH %-15s %-13s %6s %-8s %10s\n", "stage", "form", "n", "data", "cycles");

    if(BENCH_ALL)
    {
        stage_decode();
        vTaskDelay(1);
        stage_decode_psram();
        vTaskDelay(1);
        stage_cbrt();
        vTaskDelay(1);
        stage_3to3("lin_to_oklab", IN_LIN, f_lin_to_oklab, tone_fx_lin_to_oklab);
        vTaskDelay(1);
        stage_3to3("oklab_to_lin", IN_SQUEEZED, f_oklab_to_lin, tone_fx_oklab_to_lin);
        vTaskDelay(1);
        stage_lin_to_oklab_1();
        vTaskDelay(1);
        stage_squeeze();
        vTaskDelay(1);
        stage_bracket();
        vTaskDelay(1);
        stage_search();
        vTaskDelay(1);
        stage_pass_a();
        vTaskDelay(1);
        stage_plan_code();
        vTaskDelay(1);
    }
    stage_paths();
    stage_frames();

    free_all();
    heap_line("freed");
    printf("BENCH DONE\n");
}
