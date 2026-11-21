// Host probe: the graph of the tones (main/tone_graph.h) as main/tone_tables.inc holds it, through
// the queries of the header. It writes a picture of the plane, L across and C up at 2 px per 0.001
// of L and of 2C: each point of the area in the colour of its owner, the borders of the regions in
// blue, the area outside in pale cyan, a green line between each two neighbours and a yellow mark at
// each tone. It prints the figures of the graph: neighbours, triangles, the longest pair, the
// neighbours of paper and of the tone of most chroma, the smallest regions, the hue of the entries
// against the hue of the squeeze, and where the owner differs from the nearest tone in L, a and b
// with the chroma weight of the search. With NEAR.png it also writes the same plane as the nearest
// mix of tone_photo.h dithers it, every point of the area from the strip up as a target.
// Usage: out/tone_graph OUT.png [NEAR.png]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "lodepng.h"
#include "tone_graph.h"
#include "tone_photo.h"
#include "tone_tables.h"

static const int N  = static_cast<int>(TONEMAP_COUNT);
static const int PX = 2000;

static tone_graph s_g;

// The queries of the graph as tone_near_pick() asks them, as in tones.cpp.
struct near
{
    const tone_graph& g;
    unsigned          owner(int l, int c) const { return tone_graph_owner(g, l, c); }
    int               toward(unsigned a, int dl, int dc) const { return tone_graph_toward(g, a, dl, dc); }
    int               third(unsigned a, unsigned b, int side) const { return tone_graph_third(g, a, b, side); }
    int               l(unsigned e) const { return g.l[e]; }
    int               c(unsigned e) const { return g.c[e]; }
};

// Oklab to sRGB bytes, for the colour of a region.
static void srgb(const tone_entry& t, uint8_t out[3])
{
    float l = t.L + 0.3963377774f * t.a + 0.2158037573f * t.b;
    float m = t.L - 0.1055613458f * t.a - 0.0638541728f * t.b;
    float s = t.L - 0.0894841775f * t.a - 1.2914855480f * t.b;
    l = l * l * l, m = m * m * m, s = s * s * s;
    const float lin[3] = {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                          -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                          -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
    for(int k = 0; k < 3; k++)
    {
        const float v = std::min(1.0f, std::max(0.0f, lin[k]));
        const float e = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        out[k]        = static_cast<uint8_t>(std::lround(255.0f * e));
    }
}

static void name(int e)
{
    printf("%d %s (L %.4f, C %.4f)", e, k_tonemap[e].pattern, s_g.l[e] / 4096.0, s_g.c[e] / 8192.0);
}

int main(int argc, char** argv)
{
    if(argc != 2 && argc != 3)
    {
        fprintf(stderr, "usage: %s OUT.png [NEAR.png]\n", argv[0]);
        return 2;
    }
    if(!tone_graph_hydrate(&s_g, tone_tables::qL.data(), tone_tables::graph_c.data(), tone_tables::neutral.data(),
                           tone_tables::neutral_count, tone_tables::graph_ring.data(), tone_tables::graph_ring_count,
                           tone_tables::graph_cell.data(), tone_tables::graph_cand.data(), tone_tables::graph_rows))
    {
        fprintf(stderr, "the rings do not hydrate\n");
        return 1;
    }
    using namespace tone_fx_squeeze_internal;
    const int16_t* k = tone_tables::squeeze_k.data();
    // The highest c the squeeze gives at each l.
    std::vector<int> top(TONE_GRAPH_ONE + 1, -1);
    for(int L = 0; L <= 32767; L++)
    {
        const int l = tone_graph_l12(std::max(L, static_cast<int>(k[K_LB])));
        top[static_cast<size_t>(l)] = std::max(top[static_cast<size_t>(l)], tone_graph_c12(pan_edge(k, static_cast<int16_t>(L))));
    }
    const int cmax = *std::max_element(top.begin(), top.end());

    // ---- the picture
    const int            W = PX + 1, H = cmax * PX / TONE_GRAPH_ONE + 12;
    std::vector<int>     own(static_cast<size_t>(W * H), -1);
    std::vector<uint8_t> img(static_cast<size_t>(W * H * 3));
    uint8_t              col[TONEMAP_COUNT][3];
    for(int e = 0; e < N; e++) srgb(k_tonemap[e], col[e]);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if(x < 0 || x >= W || y < 0 || y >= H) return;
        uint8_t* p = &img[static_cast<size_t>((y * W + x) * 3)];
        p[0] = r, p[1] = g, p[2] = b;
    };
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++)
        {
            const int l = (x * TONE_GRAPH_ONE + PX / 2) / PX, c = ((H - 1 - y) * TONE_GRAPH_ONE + PX / 2) / PX;
            if(c > top[static_cast<size_t>(l)])
            {
                put(x, y, 215, 235, 245);
                continue;
            }
            const int e                         = static_cast<int>(tone_graph_owner(s_g, l, c));
            own[static_cast<size_t>(y * W + x)] = e;
            put(x, y, col[e][0], col[e][1], col[e][2]);
        }
    for(int y = 0; y + 1 < H; y++)
        for(int x = 0; x + 1 < W; x++)
        {
            const int o = own[static_cast<size_t>(y * W + x)];
            if(o >= 0 && ((own[static_cast<size_t>(y * W + x + 1)] >= 0 && own[static_cast<size_t>(y * W + x + 1)] != o) ||
                          (own[static_cast<size_t>((y + 1) * W + x)] >= 0 && own[static_cast<size_t>((y + 1) * W + x)] != o)))
                put(x, y, 30, 60, 220);
        }
    auto px = [&](int e, int* x, int* y) {
        *x = s_g.l[e] * PX / TONE_GRAPH_ONE;
        *y = H - 1 - s_g.c[e] * PX / TONE_GRAPH_ONE;
    };
    for(int e = 0; e < N; e++)
        for(unsigned j = s_g.at[e]; j < s_g.at[e + 1]; j++)
        {
            const int f = s_g.nb[j];
            if(f < e) continue;
            int x0, y0, x1, y1;
            px(e, &x0, &y0), px(f, &x1, &y1);
            const int n = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            for(int i = 0; i <= n; i++)
                put(x0 + (n ? (x1 - x0) * i / n : 0), y0 + (n ? (y1 - y0) * i / n : 0), 0, 150, 0);
        }
    for(int e = 0; e < N; e++)
    {
        int x, y;
        px(e, &x, &y);
        for(int dy = -3; dy <= 3; dy++)
            for(int dx = -3; dx <= 3; dx++)
            {
                const bool rim = std::abs(dx) == 3 || std::abs(dy) == 3;
                put(x + dx, y + dy, rim ? 0 : 255, rim ? 0 : 220, 0);
            }
    }
    if(lodepng_encode24_file(argv[1], img.data(), static_cast<unsigned>(W), static_cast<unsigned>(H)))
    {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    printf("wrote %s, %d x %d\n", argv[1], W, H);

    // ---- the nearest mix over the same plane: each point as a target, at the rank of the blue noise
    // of its pixel, in the colour of the tone it takes.
    if(argc == 3)
    {
        for(int y = 0; y < H; y++)
            for(int x = 0; x < W; x++)
            {
                const int l = (x * TONE_GRAPH_ONE + PX / 2) / PX, c = ((H - 1 - y) * TONE_GRAPH_ONE + PX / 2) / PX;
                if(c > top[static_cast<size_t>(l)] || c < TONE_GRAPH_STRIP)
                {
                    put(x, y, 215, 235, 245);
                    continue;
                }
                tone_near_mix m;
                tone_near_pick(near{s_g}, l, c, &m);
                const unsigned e = tone_near_choose(m, tone_tables::noise_blue[static_cast<size_t>(y & 63)][static_cast<size_t>(x & 63)] + 128);
                put(x, y, col[e][0], col[e][1], col[e][2]);
            }
        if(lodepng_encode24_file(argv[2], img.data(), static_cast<unsigned>(W), static_cast<unsigned>(H)))
        {
            fprintf(stderr, "cannot write %s\n", argv[2]);
            return 1;
        }
        printf("wrote %s, %d x %d\n", argv[2], W, H);
    }

    // ---- the figures
    int    pairs = 0, deg_max = 0, none = 0;
    double longest = 0;
    int    la = 0, lb = 0;
    for(int e = 0; e < N; e++)
    {
        const int deg = s_g.at[e + 1] - s_g.at[e];
        deg_max       = std::max(deg_max, deg);
        none += deg == 0;
        pairs += deg;
        for(unsigned j = s_g.at[e]; j < s_g.at[e + 1]; j++)
        {
            const int    f = s_g.nb[j];
            const double d = std::hypot(s_g.l[f] - s_g.l[e], s_g.c[f] - s_g.c[e]);
            if(d > longest) longest = d, la = e, lb = f;
        }
    }
    std::set<std::array<int, 3>> tri;
    for(int e = 0; e < N; e++)
    {
        const unsigned k0 = s_g.at[e], deg = s_g.at[e + 1] - k0;
        for(unsigned j = 0; j < deg; j++)
            if(s_g.fl[k0 + j] & TONE_GRAPH_TRI)
            {
                std::array<int, 3> t = {e, s_g.nb[k0 + j], s_g.nb[k0 + (j + 1) % deg]};
                std::sort(t.begin(), t.end());
                tri.insert(t);
            }
    }
    printf("%d tones, %d neighbour pairs, %zu triangles, at most %d and a mean of %.2f neighbours, %d without a "
           "region\n",
           N, pairs / 2, tri.size(), deg_max, static_cast<double>(pairs) / N, none);
    printf("longest pair, %.4f in the plane: ", longest / TONE_GRAPH_ONE);
    name(la), printf(" and "), name(lb), printf("\n");
    double closest = 1e9;
    int    ca = 0, cb = 0;
    for(int e = 0; e < N; e++)
        for(int f = e + 1; f < N; f++)
        {
            const double d = std::hypot(s_g.l[f] - s_g.l[e], s_g.c[f] - s_g.c[e]);
            if(d < closest) closest = d, ca = e, cb = f;
        }
    printf("closest two tones, %.4f in the plane: ", closest / TONE_GRAPH_ONE);
    name(ca), printf(" and "), name(cb), printf("\n");
    int paper = 0, red = 0;
    for(int e = 0; e < N; e++)
    {
        if(strcmp(k_tonemap[e].pattern, TONEMAP_PAPER_PATTERN) == 0) paper = e;
        if(s_g.c[e] > s_g.c[red]) red = e;
    }
    for(int who : {paper, red})
    {
        printf("neighbours of ");
        name(who);
        printf(":\n");
        for(unsigned j = s_g.at[who]; j < s_g.at[who + 1]; j++) printf("  "), name(s_g.nb[j]), printf("\n");
    }
    // The fewest hops from paper to the tone of most chroma.
    std::vector<int> from(static_cast<size_t>(N), -1);
    std::vector<int> q = {paper};
    from[static_cast<size_t>(paper)] = paper;
    for(size_t i = 0; i < q.size(); i++)
        for(unsigned j = s_g.at[q[i]]; j < s_g.at[q[i] + 1]; j++)
            if(from[s_g.nb[j]] < 0) from[s_g.nb[j]] = q[i], q.push_back(s_g.nb[j]);
    printf("fewest hops from paper to %d:", red);
    for(int e = red; e != paper; e = from[static_cast<size_t>(e)]) printf(" %d", e);
    printf(" %d\n", paper);

    // The share of the area each tone owns, on its lattice points from c = 0.
    std::vector<int64_t> area(static_cast<size_t>(N), 0);
    int64_t              all = 0;
    for(int l = 0; l <= TONE_GRAPH_ONE; l++)
        for(int c = 0; c <= top[static_cast<size_t>(l)]; c++) area[tone_graph_owner(s_g, l, c)]++, all++;
    std::vector<int> by(static_cast<size_t>(N));
    for(int e = 0; e < N; e++) by[static_cast<size_t>(e)] = e;
    std::sort(by.begin(), by.end(), [&](int a, int b) { return area[static_cast<size_t>(a)] < area[static_cast<size_t>(b)]; });
    printf("smallest regions, in lattice points of the area's %lld:\n", static_cast<long long>(all));
    for(int i = 0; i < 6; i++) printf("  %lld ", static_cast<long long>(area[by[i]])), name(by[static_cast<size_t>(i)]), printf("\n");

    // The hue of the coloured entries against the hue of the squeeze.
    const double hue = std::atan2(k[K_H1B], k[K_H0B]);
    double       lo = 1e9, hi = -1e9, perp = 0;
    int          neg = 0, hlo = 0, hhi = 0, hp = 0;
    for(int e = 0; e < N; e++)
    {
        const tone_entry& t = k_tonemap[e];
        if(s_g.neutral[e]) continue;
        const double h = (std::atan2(t.b, t.a) - hue) * 180 / M_PI;
        const double p = std::fabs(-t.a * std::sin(hue) + t.b * std::cos(hue));
        if(h < lo) lo = h, hlo = e;
        if(h > hi) hi = h, hhi = e;
        if(p > perp) perp = p, hp = e;
        neg += t.b < 0;
    }
    printf("hue of the squeeze %.1f degrees; coloured entries from %.1f (entry %d) to %+.1f (entry %d) degrees off it, "
           "%d with b below 0, most chroma across the hue %.4f (entry %d)\n",
           hue * 180 / M_PI, lo, hlo, hi, hhi, neg, perp, hp);

    // Where the nearest tone in L, a and b, chroma weighted by 2, of a target on the hue is another
    // than its owner.
    int64_t differ = 0, coloured = 0;
    for(int l = 0; l <= TONE_GRAPH_ONE; l++)
        for(int c = TONE_GRAPH_STRIP; c <= top[static_cast<size_t>(l)]; c++)
        {
            const double L = l / 4096.0, C = c / 8192.0, a = C * std::cos(hue), b = C * std::sin(hue);
            int          best = 0;
            double       bd   = 1e9;
            for(int e = 0; e < N; e++)
            {
                const tone_entry& t = k_tonemap[e];
                const double      d = (L - t.L) * (L - t.L) + 4 * ((a - t.a) * (a - t.a) + (b - t.b) * (b - t.b));
                if(d < bd) bd = d, best = e;
            }
            differ += best != static_cast<int>(tone_graph_owner(s_g, l, c));
            coloured++;
        }
    printf("the nearest tone in L, a and b differs from the owner at %.1f%% of the area's points from c = %d\n",
           100.0 * differ / coloured, TONE_GRAPH_STRIP);

    printf("sizes: compact %zu bytes (graph_c %zu, graph_ring %d), owner grid %zu bytes (graph_cell %zu, graph_cand "
           "%d), hydrated struct tone_graph %zu bytes\n",
           sizeof(tone_tables::graph_c) + tone_tables::graph_ring_count, sizeof(tone_tables::graph_c),
           tone_tables::graph_ring_count, sizeof(tone_tables::graph_cell) + tone_tables::graph_cand_count,
           sizeof(tone_tables::graph_cell), tone_tables::graph_cand_count, sizeof(tone_graph));
    return 0;
}
