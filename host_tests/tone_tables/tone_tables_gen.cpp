// The generator of main/tone_tables.inc: every table of the tone code that depends only on the tone
// table and fixed constants, computed here once so the firmware computes none of it at boot.
// main/tone_tables.h says where each table lives on the device.
//
// usage: tone_tables_gen OUT
//
// It writes OUT only when every build and every check passes. Else it prints the reason, writes
// nothing and exits 1. The checks:
// every pattern expands and passes every check of tone_pattern_expand() (the push limits and the
// balance of tone_plan.h), the paper and black entries exist, a neutral entry has a and b of 0, the
// kernel tables build, and an entry's linear light fits the dither's error rows.
//
// The pass A tables (the slot WHITE and tones_solid_gray()) run the kernels' stand ins of
// host_tests/tone_fx/model, which give the bits of the vector kernels, through the steps of pass_a()
// of main/tones.cpp. tone_tables_check.cpp runs pass_a() itself on the written file and compares.

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "tone_fx.h"
#include "tone_fx_bracket.h"
#include "tone_fx_bracket_internal.h"
#include "tone_fx_oklab.h"
#include "tone_fx_search_internal.h"
#include "tone_fx_squeeze.h"
#include "tone_fx_squeeze_internal.h"
#include "tone_fx_srgb.h"
#include "tone_plan.h"
#include "tone_plan_expand.h"
#include "tone_graph_build.h"
#include "tone_tables_build.h"
#include "tone_tables_source.h"
#include "tonemap.h"

namespace
{
const unsigned N = static_cast<unsigned>(TONEMAP_COUNT);

// The paper entry of tones.h.
const unsigned PAPER = 0;

// Of main/tones.cpp: the noise table side, the chunk of pass A, the neutral chroma limit in q15,
// the slot kinds and the entry of a slot that is not K_ENTRY.
const int     NOISE_N     = 64;
const int     CHUNK       = 160;
const int     NEUTRAL_C_Q = 33;
const uint8_t K_ENTRY = 0, K_NEUTRAL = 1, K_COLOUR = 2, K_WHITE = 3;
const uint8_t NO_ENTRY = 0xff;

std::string s_out;

void put(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void put(const char* fmt, ...)
{
    char    buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_out += buf;
}

[[noreturn]] void refuse(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void refuse(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "tone_tables_gen: refused, nothing written: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

// n values, 16 to a line, each line indented by ind spaces.
template <typename T> void put_list(const T* v, int n, int ind)
{
    for(int i = 0; i < n; i++)
    {
        if(i % 16 == 0) put("%*s", ind, "");
        put("%d,", static_cast<int>(v[i]));
        put(i % 16 == 15 || i == n - 1 ? "\n" : " ");
    }
}

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Blue noise, -128 to 127, uniform: ranks the cells. Each rank goes to the cell of least energy, the
// energy being a Gaussian (sigma 1.5, radius 5, wrapped) around every cell ranked so far, with hash
// values below 1e-6 to break ties. Row minima are kept so a rank costs one splat and 11 row scans.
void build_noise(int8_t out[NOISE_N][NOISE_N])
{
    const int R = 5, NN = NOISE_N * NOISE_N;
    float     g[2 * R + 1][2 * R + 1];
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
        out[y][x]           = static_cast<int8_t>(r * 256 / NN - 128);
        en[y * NOISE_N + x] = 1e9f;
        for(int dy = -R; dy <= R; dy++)
            for(int dx = -R; dx <= R; dx++)
                en[((y + dy) & (NOISE_N - 1)) * NOISE_N + ((x + dx) & (NOISE_N - 1))] += g[dy + R][dx + R];
        for(int dy = -R; dy <= R; dy++) scan((y + dy) & (NOISE_N - 1));
    }
}

// Oklab to linear sRGB in float, the conversion of tones_float_ref.inc.
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

int entry_of(const char* pattern)
{
    for(unsigned e = 0; e < N; e++)
        if(strcmp(k_tonemap[e].pattern, pattern) == 0) return static_cast<int>(e);
    return -1;
}

// L cubed, both q15, rounded to nearest. L is 0 or above.
int cube_q15(int l)
{
    return static_cast<int>((static_cast<int64_t>(l * l) * l + (1 << 29)) >> 30);
}

// One slot after pass A.
struct slot_a
{
    q15     r, g, b, L, A, B, C;
    uint8_t lo, hi, kind, ent;
};

// pass_a() of tones.cpp on a chunk whose arrays start at 0: rgb holds n pixels, n a multiple of 8,
// and 16 readable bytes past them. The slots 0 to nu - 1 are in use, the others are black.
void pass_a(const uint8_t* rgb, int n, int nu, slot_a* out)
{
    alignas(16) q15     r[CHUNK], g[CHUNK], b[CHUNK], L[CHUNK], A[CHUNK], B[CHUNK], C[CHUNK];
    alignas(16) uint8_t lo[CHUNK] = {}, hi[CHUNK] = {};
    tone_fx_srgb_to_lin(rgb, n, r, g, b);
    for(int i = nu; i < n; i++) r[i] = g[i] = b[i] = 0;
    tone_fx_lin_to_oklab(r, g, b, n, L, A, B);
    tone_fx_squeeze(L, A, B, n, L, A, B, C);
    bool colour = false, neutral = false;
    for(int i = 0; i < nu; i++)
    {
        if(C[i] < NEUTRAL_C_Q) neutral = true;
        else colour = true;
    }
    if(colour) tone_fx_oklab_to_lin(L, A, B, n, r, g, b);
    if(neutral) tone_fx_bracket(L, n, lo, hi);
    for(int i = 0; i < nu; i++)
    {
        slot_a& s = out[i];
        s.ent     = NO_ENTRY;
        if(C[i] >= NEUTRAL_C_Q) s.kind = K_COLOUR;
        else if(lo[i] == hi[i])
        {
            s.kind = K_ENTRY;
            s.ent  = lo[i];
        }
        else
        {
            s.kind = K_NEUTRAL;
            r[i]   = static_cast<q15>(cube_q15(L[i]));
        }
        s.r = r[i], s.g = g[i], s.b = b[i], s.L = L[i], s.A = A[i], s.B = B[i], s.C = C[i];
        s.lo = lo[i], s.hi = hi[i];
    }
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        fprintf(stderr, "usage: %s OUT\n", argv[0]);
        return 2;
    }

    // ---- the entries
    if(strcmp(k_tonemap[PAPER].pattern, TONEMAP_PAPER_PATTERN) != 0)
        refuse("entry %u is not the paper pattern %s", PAPER, TONEMAP_PAPER_PATTERN);
    const int black = entry_of(TONEMAP_BLACK_PATTERN);
    if(black < 0) refuse("no entry has the black pattern %s", TONEMAP_BLACK_PATTERN);
    static int16_t qL[TONEMAP_COUNT], qlin[TONEMAP_COUNT][3];
    int            lin_min = 0, lin_max = 0;
    for(unsigned e = 0; e < N; e++)
    {
        const tone_entry& t = k_tonemap[e];
        const float       lab[3] = {t.L, t.a, t.b};
        float             lin[3];
        oklab_to_lin(lab, lin);
        // Any r or R pull makes a pink, so only a pattern without them is neutral. The table gives
        // such an entry a and b of 0, and the neutral path takes its light as L cubed.
        if(!strpbrk(t.pattern, "rR"))
        {
            if(t.a != 0.0f || t.b != 0.0f)
                refuse("neutral entry %u (%s) has a %.4f b %.4f, not 0", e, t.pattern, static_cast<double>(t.a),
                       static_cast<double>(t.b));
            lin[0] = lin[1] = lin[2] = t.L * t.L * t.L;
        }
        qL[e] = q15_from(t.L);
        for(int k = 0; k < 3; k++)
        {
            qlin[e][k] = q15_from(lin[k]);
            lin_min    = std::min(lin_min, static_cast<int>(qlin[e][k]));
            lin_max    = std::max(lin_max, static_cast<int>(qlin[e][k]));
        }
    }
    // A residual is at most Q15_ONE - lin_min or lin_max. Atkinson puts at most 6/8 of the largest
    // residual into a cell, so the error rows hold it in 16 bits.
    if((std::max(Q15_ONE - lin_min, lin_max) * 6 + 7) / 8 + 8 > 32767)
        refuse("an entry's linear light runs past the error range");

    // ---- the kernels
    int16_t squeeze_k[tone_fx_squeeze_internal::K_COUNT];
    float   fit_img = 0.0f, fit_pan = 0.0f, fit_curve = 0.0f;
    if(!tone_fx_squeeze_build(squeeze_k, &fit_img, &fit_pan, &fit_curve))
        refuse("the squeeze parameters do not build");
    if(!tone_fx_bracket_build()) refuse("the bracket table does not build");
    static tone_fx_search_internal::table search;
    int                                   search_max[3];
    if(!tone_fx_search_build(&search, search_max)) refuse("the search table does not build");

    static int8_t noise[NOISE_N][NOISE_N];
    build_noise(noise);

    // ---- pass A: the slot WHITE, from 8 white pixels with slot 0 in use
    slot_a  w[8];
    uint8_t px[3 * CHUNK + 16];
    memset(px, 255, sizeof(px));
    pass_a(px, 8, 1, w);
    tone_white_slot white = {w[0].r, w[0].g, w[0].b, w[0].L, w[0].A,    w[0].B,   w[0].C,
                             w[0].lo, w[0].hi, w[0].kind, w[0].ent, K_ENTRY};
    if(w[0].kind != K_ENTRY || w[0].lo != PAPER)
    {
        white.white_kind = w[0].kind;
        white.kind       = K_WHITE;
    }
    white.ent = white.kind == K_ENTRY ? white.lo : NO_ENTRY;

    // ---- pass A on every gray byte, in chunks
    tone_gray_target gray[256];
    static_assert(256 % CHUNK % 8 == 0, "the gray runs are whole vectors");
    for(int v0 = 0; v0 < 256; v0 += CHUNK)
    {
        const int n = std::min(CHUNK, 256 - v0);
        slot_a    s[CHUNK];
        memset(px, 0, sizeof(px));
        for(int i = 0; i < 3 * n; i++) px[i] = static_cast<uint8_t>(v0 + i / 3);
        pass_a(px, n, n, s);
        for(int i = 0; i < n; i++) gray[v0 + i] = {s[i].L, s[i].kind == K_ENTRY ? s[i].lo : -1};
    }
    uint8_t solid[256];
    tone_solid_gray_table(gray, qL, solid);

    // ---- the photo dither: the neutral entries and the coloured ones on the red side of the
    // neutral axis, triangulated in the plane of tone_photo_yv()
    static int16_t photo_yv[2 * TONEMAP_COUNT];
    bool           photo_use[TONEMAP_COUNT];
    for(unsigned e = 0; e < N; e++)
    {
        int y, v;
        tone_photo_yv(qlin[e][0], qlin[e][1], qlin[e][2], &y, &v);
        photo_yv[2 * e]     = static_cast<int16_t>(y);
        photo_yv[2 * e + 1] = static_cast<int16_t>(v);
        photo_use[e]   = !strpbrk(k_tonemap[e].pattern, "rR") || v > 0;
    }
    std::vector<tone_tri> photo_tri;
    std::vector<uint8_t>  photo_top;
    char                  photo_why[160];
    if(!tone_photo_build(photo_yv, photo_use, static_cast<int>(N), photo_tri, photo_top, photo_why,
                         sizeof(photo_why)))
        refuse("the photo triangles do not build: %s", photo_why);
    int photo_clamped = 0;
    // The start grid from the coloured targets of sRGB colours on a step of 5 through pass A.
    std::vector<tone_photo_sample> photo_s;
    {
        std::vector<uint8_t> all;
        for(int r = 0; r < 256; r += 5)
            for(int g = 0; g < 256; g += 5)
                for(int b = 0; b < 256; b += 5) all.insert(all.end(), {uint8_t(r), uint8_t(g), uint8_t(b)});
        for(size_t p0 = 0; p0 < all.size() / 3; p0 += CHUNK)
        {
            const int n = static_cast<int>(std::min<size_t>(CHUNK, all.size() / 3 - p0));
            slot_a    s[CHUNK];
            memset(px, 0, sizeof(px));
            memcpy(px, &all[3 * p0], static_cast<size_t>(3 * n));
            pass_a(px, tone_fx_pad8(n), n, s);
            for(int i = 0; i < n; i++)
            {
                if(s[i].kind != K_COLOUR) continue;
                int y, v;
                tone_photo_yv(s[i].r, s[i].g, s[i].b, &y, &v);
                const int vc = tone_photo_clamp(photo_top.data(), static_cast<int>(photo_top.size()), photo_yv, y, v);
                photo_clamped += vc != v;
                photo_s.push_back({y, vc, tone_photo_cell(s[i].L, s[i].C)});
            }
        }
    }
    static uint8_t photo_grid[TONE_PHOTO_GRID * TONE_PHOTO_GRID];
    int            photo_out = 0;
    if(!tone_photo_grid(photo_tri, photo_yv, photo_s, photo_grid, &photo_out) || photo_out)
        refuse("%d coloured targets lie outside the photo triangles after the clamp", photo_out);

    // ---- the patterns
    static tone_seq  seq[TONEMAP_COUNT];
    static tone_push push[TONEMAP_COUNT];
    for(unsigned e = 0; e < N; e++)
    {
        char why[192];
        if(!tone_pattern_expand(k_tonemap[e].pattern, &seq[e], why, sizeof(why)))
            refuse("tone map entry %u: %s", e, why);
        tone_push_of(seq[e], &push[e]);
    }

    // ---- the graph of the tones in the plane of L and 2C
    std::string graph_inc;
    char        graph_why[192];
    if(!tone_graph_build(qL, squeeze_k, graph_inc, graph_why, sizeof(graph_why)))
        refuse("the graph of the tones does not build: %s", graph_why);

    // ---- the file
    put("// main/tone_tables.inc, generated by host_tests/tone_tables (tone_tables_gen). Do not edit.\n"
        "//\n"
        "// The tables of the tone code that depend only on the tone table and fixed constants, built on\n"
        "// the host from main/tonemap.h and main/tonemap_entries.inc, the phase shapes and push limits of\n"
        "// components/epd97, the kernel constants of main/tone_fx_*_internal.h and the build code of\n"
        "// host_tests/tone_tables. main/tone_tables.h includes this file and says where each table lives on\n"
        "// the device. To regenerate: make generate in host_tests/tone_tables. make test there fails while\n"
        "// this file differs from a fresh run.\n"
        "//\n"
        "// Squeeze edge fit, largest miss against the float tables over every q15 L: image %.6f, panel\n"
        "// %.6f. Tone curve at gamma %g, largest miss against the power over every q15 L: %.6f.\n\n",
        static_cast<double>(fit_img), static_cast<double>(fit_pan), static_cast<double>(TONEMAP_GAMMA),
        static_cast<double>(fit_curve));
    put("namespace tone_tables\n{\n");
    put("inline constexpr unsigned count       = %u;\n", N);
    put("inline constexpr uint64_t source_hash = 0x%016" PRIx64 "ull;\n\n", tone_tables_source_hash());

    put("// The squeeze parameters in the order of tone_fx_squeeze_internal.h, the edge fit and the tone\n"
        "// curve fit.\n");
    put("inline constexpr std::array<int16_t, tone_fx_squeeze_internal::K_COUNT> squeeze_k = {{\n");
    put_list(squeeze_k, tone_fx_squeeze_internal::K_COUNT, 4);
    put("}};\n");
    put("inline constexpr float squeeze_fit_img   = %.9gf;\n", static_cast<double>(fit_img));
    put("inline constexpr float squeeze_fit_pan   = %.9gf;\n", static_cast<double>(fit_pan));
    put("inline constexpr float squeeze_fit_curve = %#.9gf;\n\n", static_cast<double>(fit_curve));

    const int n_neutral = tone_fx_neutral_count();
    const int steps     = tone_fx_bracket_steps();
    put("// The neutral entries, lightest first, and the bracket step table (tone_fx_bracket_internal.h).\n");
    put("inline constexpr int neutral_count = %d;\n", n_neutral);
    put("inline constexpr std::array<uint8_t, neutral_count> neutral = {{\n");
    put_list(tone_fx_neutral_entries(), n_neutral, 4);
    put("}};\n");
    put("inline constexpr int bracket_steps = %d;\n", steps);
    put("inline constexpr std::array<int16_t, 2 + 2 * bracket_steps> bracket = {{\n");
    put_list(tone_fx_bracket_table(), 2 + 2 * steps, 4);
    put("}};\n\n");

    put("// The search table (tone_fx_search_internal.h): per block a, L, b and tag of 8 entries.\n");
    put("inline constexpr tone_fx_search_internal::table search = {\n");
    put("    {%d, %d, %d, %d},\n", search.none[0], search.none[1], search.none[2], search.none[3]);
    put("    {%d, %d, %d, %d},\n", search.mask[0], search.mask[1], search.mask[2], search.mask[3]);
    put("    {\n");
    for(int k = 0; k < tone_fx_search_internal::N_BLOCK; k++)
    {
        const tone_fx_search_internal::block& b = search.blk[k];
        put("        {{");
        for(int i = 0; i < 8; i++) put("%d%s", b.a[i], i < 7 ? ", " : "}, {");
        for(int i = 0; i < 8; i++) put("%d%s", b.l[i], i < 7 ? ", " : "}, {");
        for(int i = 0; i < 8; i++) put("%d%s", b.b[i], i < 7 ? ", " : "}, {");
        for(int i = 0; i < 8; i++) put("%d%s", b.tag[i], i < 7 ? ", " : "}},\n");
    }
    put("    },\n};\n");
    put("// The highest L, 2A and 2B of a search point.\n");
    put("inline constexpr std::array<int, 3> search_max = {{%d, %d, %d}};\n\n", search_max[0], search_max[1],
        search_max[2]);

    put("// Per entry, q15: L, and the linear light, L cubed on all three channels for a neutral entry.\n");
    put("inline constexpr std::array<int16_t, count> qL = {{\n");
    put_list(qL, static_cast<int>(N), 4);
    put("}};\ninline constexpr std::array<std::array<int16_t, 3>, count> qlin = {{\n");
    for(unsigned e = 0; e < N; e++) put("    {{%d, %d, %d}},\n", qlin[e][0], qlin[e][1], qlin[e][2]);
    put("}};\n\n");

    put("// Blue noise, %d x %d, -128 to 127, uniform.\n", NOISE_N, NOISE_N);
    put("inline constexpr std::array<std::array<int8_t, %d>, %d> noise_blue = {{\n", NOISE_N, NOISE_N);
    for(int y = 0; y < NOISE_N; y++)
    {
        put("    {{\n");
        put_list(noise[y], NOISE_N, 8);
        put("    }},\n");
    }
    put("}};\n\n");

    put("// The slot WHITE: r, g, b, L, A, B, C, lo, hi, kind, ent, white_kind.\n");
    put("inline constexpr tone_white_slot white = {%d, %d, %d, %d, %d, %d, %d, %u, %u, %u, %u, %u};\n\n", white.r,
        white.g, white.b, white.L, white.A, white.B, white.C, white.lo, white.hi, white.kind, white.ent,
        white.white_kind);

    put("// The photo dither (tone_photo.h): per entry Y and V, the triangles (entries darkest first, the\n"
        "// triangle across the edge opposite each, the orientation), and per cell of L and C the triangle a\n"
        "// walk starts from.\n");
    put("inline constexpr std::array<int16_t, 2 * count> photo_yv = {{\n");
    for(unsigned e = 0; e < N; e++) put("    %d, %d,\n", photo_yv[2 * e], photo_yv[2 * e + 1]);
    put("}};\n");
    put("inline constexpr int photo_tri_count = %d;\n", static_cast<int>(photo_tri.size()));
    put("inline constexpr std::array<tone_tri, photo_tri_count> photo_tri = {{\n");
    for(const tone_tri& t : photo_tri)
        put("    {{%u, %u, %u}, {%u, %u, %u}, %d, 0},\n", t.v[0], t.v[1], t.v[2], t.nb[0], t.nb[1], t.nb[2], t.sign);
    put("}};\n");
    put("inline constexpr int photo_top_count = %d;\n", static_cast<int>(photo_top.size()));
    put("inline constexpr std::array<uint8_t, photo_top_count> photo_top = {{\n");
    put_list(photo_top.data(), static_cast<int>(photo_top.size()), 4);
    put("}};\n");
    put("inline constexpr std::array<uint8_t, %d> photo_grid = {{\n", TONE_PHOTO_GRID * TONE_PHOTO_GRID);
    put_list(photo_grid, TONE_PHOTO_GRID * TONE_PHOTO_GRID, 4);
    put("}};\n\n");

    put("// tones_solid_gray() of each gray byte.\n");
    put("inline constexpr std::array<uint8_t, 256> solid_gray = {{\n");
    put_list(solid, 256, 4);
    put("}};\n\n");

    put("// The expanded patterns: p1a, p1b, p2, n1a, n1b, n2, net, prefix, nudge, residual, split.\n");
    put("inline constexpr std::array<tone_seq, count> seq = {{\n");
    for(unsigned e = 0; e < N; e++)
    {
        const tone_seq& s = seq[e];
        put("    {\"%s\", \"%s\", \"%s\", %u, %u, %u, %d, %d, %d, %d, %d}, // %u %s\n", s.p1a, s.p1b, s.p2, s.n1a,
            s.n1b, s.n2, s.net, s.prefix, s.nudge, s.residual, s.split, e, k_tonemap[e].pattern);
    }
    put("}};\n");
    put("// Their push figures (tone_push_of()): vdh, vdl, vdhr, pos, max_vdh, max_vdl, max_vdhr, max_pos.\n");
    put("inline constexpr std::array<tone_push, count> push = {{\n");
    for(unsigned e = 0; e < N; e++)
    {
        const tone_push& u = push[e];
        put("    {%u, %u, %u, %u, %u, %u, %u, %u},\n", u.vdh, u.vdl, u.vdhr, u.pos, u.max_vdh, u.max_vdl, u.max_vdhr,
            u.max_pos);
    }
    put("}};\n");
    s_out += "\n" + graph_inc;
    put("} // namespace tone_tables\n");

    FILE* f = fopen(argv[1], "w");
    if(!f || fwrite(s_out.data(), 1, s_out.size(), f) != s_out.size() || fclose(f) != 0)
    {
        fprintf(stderr, "tone_tables_gen: cannot write %s\n", argv[1]);
        return 1;
    }
    printf("tone_tables_gen: %u entries, %d neutral, %d bracket steps, squeeze fit image %.6f panel %.6f "
           "curve %.6f, white kind %u entry %u, %zu photo triangles, %d of %zu coloured targets above the hull, "
           "wrote %s\n",
           N, n_neutral, steps, static_cast<double>(fit_img), static_cast<double>(fit_pan),
           static_cast<double>(fit_curve), white.kind, white.ent, photo_tri.size(), photo_clamped, photo_s.size(),
           argv[1]);
    return 0;
}
