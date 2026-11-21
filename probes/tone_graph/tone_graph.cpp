// Host probe: the graph of the tones (main/tone_graph.h) as main/tone_tables.inc holds it, through
// the queries of the header. It writes a picture of the plane, L across and C up at 2 px per 0.001
// of L and of 2C: each point of the area in the colour of its owner, the borders of the regions in
// blue, the area outside in pale cyan, a green line between each two neighbours and a yellow mark at
// each tone. It prints the figures of the graph: neighbours, triangles, the longest pair, the
// neighbours of paper and of the tone of most chroma, the smallest regions, the hue of the entries
// against the hue of the squeeze, and where the owner differs from the nearest tone in L, a and b
// with the chroma weight of the search. With NEAR.png it also writes the same plane as the nearest
// mix of tone_photo.h dithers it, every point of the area from the strip up as a target. Over the
// same targets it measures the nearest mix of the firmware: the distance from each target to the
// point its mix reaches by where the target lies, the jumps of that point between adjacent targets,
// and the share of the dots each tone takes, and it writes OUT.png.miss.png, the plane in the colour
// of where each target lies. OUT_cells.png holds the borders, the neighbour lines and the tone marks
// of OUT.png alone on a clear ground, to lay over NEAR.png. Last it lists the triangles of the plain
// Delaunay triangulation of the tone points that the graph lacks, with the targets of the area in
// each. The mix takes the firmware's default ease, or EASE="solid_t solid_w line_t line_w" from the
// environment. NEAR_plain.png is the same dither with both steps of the ease off, and
// NEAR_closest.png the same with the closest rule, the nearest point of the rim, for targets outside
// the triangles in place of the focal rule. The figures also hold a search of the focal point.
// AT="x0 y0 x1 y1 n" in the environment prints the mix at n + 1 pixels of the picture from (x0, y0)
// to (x1, y1) in place of the figures.
// Usage: out/tone_graph OUT.png [NEAR.png]
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "lodepng.h"
#include "tone_graph.h"
#include "tone_photo.h"
#include "tone_tables.h"
#include "tones.h"

static const int N  = static_cast<int>(TONEMAP_COUNT);
static const int PX = 2000;

static tone_graph s_g;

// The queries of the graph as tone_near_pick() asks them, as in tones.cpp.
struct near
{
    const tone_graph& g;
    int               top(int l) const { return tone_graph_top(g, l); }
    unsigned          owner(int l, int c) const { return tone_graph_owner(g, l, c); }
    int               toward(unsigned a, int dl, int dc) const { return tone_graph_toward(g, a, dl, dc); }
    int               third(unsigned a, unsigned b, int side) const { return tone_graph_third(g, a, b, side); }
    bool              unit(unsigned a, unsigned b, int* ul, int* uc) const { return tone_graph_unit(g, a, b, ul, uc); }
    void              focal(int l, int c, uint8_t t[3], int32_t w[3]) const
    {
        int k;
        tone_graph_focal(g, l, c, &k, w);
        t[0] = g.top_e[k], t[1] = t[2] = g.top_e[k + 1], w[2] = 0;
    }
    void rim(int l, int c, uint8_t t[3], int32_t w[3]) const
    {
        int k;
        tone_graph_rim(g, l, c, &k, w);
        t[0] = g.rim_a[k], t[1] = g.rim_b[k], t[2] = g.rim_t[k] == TONE_GRAPH_END ? g.rim_b[k] : g.rim_t[k], w[2] = 0;
    }
    int l(unsigned e) const { return g.l[e]; }
    int c(unsigned e) const { return g.c[e]; }
};

// The ease of the pick: the firmware's defaults, or EASE="solid_t solid_w line_t line_w" in the
// environment.
static tone_near_ease s_ease = tone_near_ease_of(TONE_NEAR_SOLID_T, TONE_NEAR_SOLID_W, TONE_NEAR_LINE_T, TONE_NEAR_LINE_W);

// The mix of the firmware for the target (l, c).
static void pick(int l, int c, tone_near_mix* m)
{
    tone_near_pick(near{s_g}, s_ease, l, c, m);
}

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
                           tone_tables::graph_top.data(), tone_tables::graph_top_count, tone_tables::graph_cell.data(),
                           tone_tables::graph_cand.data(), tone_tables::graph_rows))
    {
        fprintf(stderr, "the rings or the top do not hydrate\n");
        return 1;
    }
    if(const char* ez = getenv("EASE"))
    {
        int st, sw, lt, lw;
        if(sscanf(ez, "%d %d %d %d", &st, &sw, &lt, &lw) == 4) s_ease = tone_near_ease_of(st, sw, lt, lw);
    }
    printf("ease solid %d %d line %d %d\n", s_ease.solid.t, s_ease.solid.w, s_ease.line.t, s_ease.line.w);
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
    // The borders, the neighbour lines and the tone marks go to the picture and, opaque on a clear
    // ground, to the layer of cells.
    std::vector<uint8_t> cells(static_cast<size_t>(W * H * 4), 0);
    auto                 mark = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if(x < 0 || x >= W || y < 0 || y >= H) return;
        put(x, y, r, g, b);
        uint8_t* p = &cells[static_cast<size_t>((y * W + x) * 4)];
        p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
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
                mark(x, y, 30, 60, 220);
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
                mark(x0 + (n ? (x1 - x0) * i / n : 0), y0 + (n ? (y1 - y0) * i / n : 0), 0, 150, 0);
        }
    for(int e = 0; e < N; e++)
    {
        int x, y;
        px(e, &x, &y);
        for(int dy = -3; dy <= 3; dy++)
            for(int dx = -3; dx <= 3; dx++)
            {
                const bool rim = std::abs(dx) == 3 || std::abs(dy) == 3;
                mark(x + dx, y + dy, rim ? 0 : 255, rim ? 0 : 220, 0);
            }
    }
    if(lodepng_encode24_file(argv[1], img.data(), static_cast<unsigned>(W), static_cast<unsigned>(H)))
    {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    printf("wrote %s, %d x %d\n", argv[1], W, H);
    std::string cells_png = argv[1];
    if(cells_png.size() > 4 && cells_png.compare(cells_png.size() - 4, 4, ".png") == 0) cells_png.resize(cells_png.size() - 4);
    cells_png += "_cells.png";
    if(lodepng_encode32_file(cells_png.c_str(), cells.data(), static_cast<unsigned>(W), static_cast<unsigned>(H)))
    {
        fprintf(stderr, "cannot write %s\n", cells_png.c_str());
        return 1;
    }
    printf("wrote %s, %d x %d, the borders, neighbour lines and tones on a clear ground\n", cells_png.c_str(), W, H);

    // ---- the nearest mix over the same plane: each point as a target, at the rank of the blue noise
    // of its pixel, in the colour of the tone it takes.
    if(argc == 3)
    {
        std::string stem = argv[2];
        if(stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".png") == 0) stem.resize(stem.size() - 4);
        const std::string    plain = stem + "_plain.png", closest = stem + "_closest.png";
        const tone_near_ease keep  = s_ease;
        for(int pass = 0; pass < 3; pass++)
        {
            s_ease = pass == 1 ? tone_near_ease_of(0, 0, 0, 0) : keep;
            if(pass == 2) s_ease.closest = true;
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
                    pick(l, c, &m);
                    const unsigned e = tone_near_choose(m, tone_tables::noise_blue[static_cast<size_t>(y & 63)][static_cast<size_t>(x & 63)] + 128);
                    put(x, y, col[e][0], col[e][1], col[e][2]);
                }
            const char* path = pass == 0 ? argv[2] : pass == 1 ? plain.c_str() : closest.c_str();
            if(lodepng_encode24_file(path, img.data(), static_cast<unsigned>(W), static_cast<unsigned>(H)))
            {
                fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            printf("wrote %s, %d x %d, ease solid %d %d line %d %d, %s rule outside the triangles\n", path, W, H,
                   s_ease.solid.t, s_ease.solid.w, s_ease.line.t, s_ease.line.w, s_ease.closest ? "closest" : "focal");
        }
        s_ease = keep;
    }

    // ---- AT="x0 y0 x1 y1 n" in the environment prints the mix at n + 1 pixels of the picture from
    // (x0, y0) to (x1, y1).
    if(const char* at = getenv("AT"))
    {
        int x0, y0, x1, y1, n;
        if(sscanf(at, "%d %d %d %d %d", &x0, &y0, &x1, &y1, &n) == 5 && n > 0)
            for(int i = 0; i <= n; i++)
            {
                const int x = x0 + (x1 - x0) * i / n, y = y0 + (y1 - y0) * i / n;
                const int l = (x * TONE_GRAPH_ONE + PX / 2) / PX, c = ((H - 1 - y) * TONE_GRAPH_ONE + PX / 2) / PX;
                tone_near_mix m;
                pick(l, c, &m);
                const double s = static_cast<double>(m.w[0] + m.w[1] + m.w[2]);
                printf("x %4d y %3d  l %4d c %4d  area top %4d  triangle top %4d  owner %2u  how %d  steps %d ", x, y, l, c,
                       top[static_cast<size_t>(l)], tone_graph_top(s_g, l), tone_graph_owner(s_g, l, c), m.how, m.steps);
                for(int j = 0; j < 3; j++) printf(" %2d (%4d,%4d) %.3f", m.v[j], s_g.l[m.v[j]], s_g.c[m.v[j]], m.w[j] / s);
                printf("\n");
            }
        return 0;
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

    // ---- where the nearest mix misses a target that three tones of a triangle hold.
    {
        // The upper hull of the tones over l: above it no mix of tones reaches a target.
        std::vector<int> hs(static_cast<size_t>(N));
        for(int e = 0; e < N; e++) hs[static_cast<size_t>(e)] = e;
        std::sort(hs.begin(), hs.end(), [&](int a, int b) { return s_g.l[a] != s_g.l[b] ? s_g.l[a] < s_g.l[b] : s_g.c[a] < s_g.c[b]; });
        std::vector<int> hull;
        for(int e : hs)
        {
            while(hull.size() >= 2)
            {
                const int     p = hull[hull.size() - 2], q2 = hull.back();
                const int64_t x = static_cast<int64_t>(s_g.l[q2] - s_g.l[p]) * (s_g.c[e] - s_g.c[p]) -
                                  static_cast<int64_t>(s_g.c[q2] - s_g.c[p]) * (s_g.l[e] - s_g.l[p]);
                if(x < 0) break;
                hull.pop_back();
            }
            hull.push_back(e);
        }
        auto under_hull = [&](int l, int c) {
            for(size_t i = 0; i + 1 < hull.size(); i++)
            {
                const int p = hull[i], q2 = hull[i + 1];
                if(l < s_g.l[p] || l > s_g.l[q2] || s_g.l[p] == s_g.l[q2]) continue;
                return static_cast<int64_t>(c - s_g.c[p]) * (s_g.l[q2] - s_g.l[p]) <=
                       static_cast<int64_t>(s_g.c[q2] - s_g.c[p]) * (l - s_g.l[p]);
            }
            return false;
        };
        auto inside = [&](const std::array<int, 3>& t, int l, int c) {
            int pos = 0, neg2 = 0;
            for(int i = 0; i < 3; i++)
            {
                const int     p = t[static_cast<size_t>(i)], q2 = t[static_cast<size_t>((i + 1) % 3)];
                const int64_t x = static_cast<int64_t>(s_g.l[q2] - s_g.l[p]) * (c - s_g.c[p]) -
                                  static_cast<int64_t>(s_g.c[q2] - s_g.c[p]) * (l - s_g.l[p]);
                pos += x > 0, neg2 += x < 0;
            }
            return !(pos && neg2);
        };
        enum { OUT_HULL, NO_TRI, HIT, MISS_A, MISS_B, MISS_C, NCAT };
        const char*          cat_name[NCAT] = {"above the hull of the tones, no mix reaches it",
                                               "under the hull, in no triangle of the graph",
                                               "in a triangle, the mix reaches it",
                                               "in a triangle, miss: the owner is not a corner of it",
                                               "in a triangle, miss: the owner is a corner, the tone toward the target is not",
                                               "in a triangle, miss: both are corners, the third tone is another or none"};
        const uint8_t        cat_col[NCAT][3] = {{120, 120, 120}, {255, 160, 0}, {235, 235, 235}, {200, 0, 200}, {220, 0, 0}, {0, 90, 220}};
        int64_t              cnt[NCAT] = {};
        double               esum[NCAT] = {}, emax[NCAT] = {};
        std::vector<uint8_t> cat(static_cast<size_t>(W * H), 255);
        const std::vector<std::array<int, 3>> tris(tri.begin(), tri.end());
        size_t               last = 0;
        int64_t              jumps[NCAT] = {}, far_n = 0, far_over = 0;
        double               far_sum = 0, far_max = 0;
        std::vector<double>  share(static_cast<size_t>(N), 0.0);
        auto                 mixpt = [&](int l, int c, double* ml, double* mc) {
            tone_near_mix m;
            pick(l, c, &m);
            const double s = static_cast<double>(m.w[0] + m.w[1] + m.w[2]);
            *ml = *mc = 0;
            for(int i = 0; i < 3; i++) share[m.v[i]] += m.w[i] / s;
            for(int i = 0; i < 3; i++) *ml += m.w[i] / s * s_g.l[m.v[i]], *mc += m.w[i] / s * s_g.c[m.v[i]];
        };
        for(int l = 0; l <= TONE_GRAPH_ONE; l++)
            for(int c = TONE_GRAPH_STRIP; c <= top[static_cast<size_t>(l)]; c++)
            {
                double ml, mc;
                mixpt(l, c, &ml, &mc);
                const double err = std::hypot(ml - l, mc - c) / TONE_GRAPH_ONE;
                int          k2  = OUT_HULL;
                if(under_hull(l, c))
                {
                    k2 = NO_TRI;
                    size_t f = tris.size();
                    if(inside(tris[last], l, c)) f = last;
                    for(size_t i = 0; f == tris.size() && i < tris.size(); i++)
                        if(inside(tris[i], l, c)) f = i;
                    if(f < tris.size())
                    {
                        last = f;
                        const unsigned a  = tone_graph_owner(s_g, l, c);
                        const int      b  = tone_graph_toward(s_g, a, l - s_g.l[a], c - s_g.c[a]);
                        auto           in = [&](int e) { return tris[f][0] == e || tris[f][1] == e || tris[f][2] == e; };
                        k2 = err <= 0.002 ? HIT : !in(static_cast<int>(a)) ? MISS_A : !in(b) ? MISS_B : MISS_C;
                    }
                }
                cnt[k2]++, esum[k2] += err, emax[k2] = std::max(emax[k2], err);
                // A jump: the mix point moves over 0.01 between this target and the next in l or c.
                double nl, nc;
                bool   jump = false;
                if(l < TONE_GRAPH_ONE && c <= top[static_cast<size_t>(l + 1)])
                    mixpt(l + 1, c, &nl, &nc), jump = jump || std::hypot(nl - ml, nc - mc) > 0.01 * TONE_GRAPH_ONE;
                if(c < top[static_cast<size_t>(l)])
                    mixpt(l, c + 1, &nl, &nc), jump = jump || std::hypot(nl - ml, nc - mc) > 0.01 * TONE_GRAPH_ONE;
                jumps[k2] += jump;
                // The nearest point of all triangles of the graph, against the mix.
                if(k2 == NO_TRI || k2 == OUT_HULL)
                {
                    double best = 1e18;
                    for(const auto& t : tris)
                    {
                        const uint8_t tv[3] = {static_cast<uint8_t>(t[0]), static_cast<uint8_t>(t[1]), static_cast<uint8_t>(t[2])};
                        int32_t       w[3];
                        tone_near_area(near{s_g}, tv, l, c, w);
                        const double sw = static_cast<double>(w[0] + w[1] + w[2]);
                        double       tl = 0, tc = 0;
                        for(int i = 0; i < 3; i++) tl += w[i] / sw * s_g.l[t[static_cast<size_t>(i)]], tc += w[i] / sw * s_g.c[t[static_cast<size_t>(i)]];
                        best = std::min(best, std::hypot(tl - l, tc - c));
                    }
                    const double over = err - best / TONE_GRAPH_ONE;
                    far_n++, far_over += over > 0.002, far_sum += over, far_max = std::max(far_max, over);
                }
                const int x = l * PX / TONE_GRAPH_ONE, y = H - 1 - c * PX / TONE_GRAPH_ONE;
                if(x >= 0 && x < W && y >= 0 && y < H) cat[static_cast<size_t>(y * W + x)] = static_cast<uint8_t>(k2);
            }
        int64_t tot = 0;
        for(int i = 0; i < NCAT; i++) tot += cnt[i];
        printf("the nearest mix with the ease solid %d %d line %d %d against the target, over %lld points from c = %d, "
               "error in the plane. A miss is a mix over 0.002 from the target, which the ease makes of a target near a "
               "tone or an edge, and a jump a move of the mix over 0.01 from the next target in l or c:\n",
               s_ease.solid.t, s_ease.solid.w, s_ease.line.t, s_ease.line.w, static_cast<long long>(tot), TONE_GRAPH_STRIP);
        for(int i = 0; i < NCAT; i++)
            printf("  %5.1f%%  mean %.4f  worst %.4f  jumps %6lld  %s\n", 100.0 * cnt[i] / tot, cnt[i] ? esum[i] / cnt[i] : 0.0, emax[i],
                   static_cast<long long>(jumps[i]), cat_name[i]);
        printf("of the %lld targets in no triangle, the mix lies over 0.002 farther than the nearest point of the triangles at "
               "%.1f%%, mean excess %.4f, worst %.4f\n",
               static_cast<long long>(far_n), far_n ? 100.0 * far_over / far_n : 0.0, far_n ? far_sum / far_n : 0.0, far_max);
        // The share of all dots each tone takes. Each target is counted up to three times, the same
        // for every tone.
        double all_share = 0;
        int    used      = 0;
        for(int e = 0; e < N; e++) all_share += share[static_cast<size_t>(e)], used += share[static_cast<size_t>(e)] > 0;
        printf("%d of %d tones take dots. Share of the dots per tone:\n", used, N);
        for(int e = 0; e < N; e++) printf("  %6.3f%% ", 100 * share[static_cast<size_t>(e)] / all_share), name(e), printf("\n");
        const std::string miss = std::string(argv[1]) + ".miss.png";
        for(int y = 0; y < H; y++)
            for(int x = 0; x < W; x++)
            {
                const uint8_t k2 = cat[static_cast<size_t>(y * W + x)];
                if(k2 == 255) put(x, y, 255, 255, 255);
                else put(x, y, cat_col[k2][0], cat_col[k2][1], cat_col[k2][2]);
            }
        if(!lodepng_encode24_file(miss.c_str(), img.data(), static_cast<unsigned>(W), static_cast<unsigned>(H)))
            printf("wrote %s: gray above the hull, orange no triangle, light hit, magenta owner not a corner, red toward "
                   "not a corner, blue third\n",
                   miss.c_str());
    }

    // ---- the focal point F of the focal rule: the targets above the top, more than 2 above it, take
    // the point where the segment from them to F crosses the top chain. Per F of a grid, whether the
    // chain is star shaped from F (every edge turns the same way seen from F), and the mean and worst
    // distance from target to that point, against the closest rule, the nearest point of the rim, and
    // against the target with its c lowered to the top.
    {
        const int                              nk = s_g.top_n;
        std::vector<std::pair<int, int>>       outside;
        for(int l = 0; l <= TONE_GRAPH_ONE; l++)
            for(int c = TONE_GRAPH_STRIP; c <= top[static_cast<size_t>(l)]; c++)
                if(c > tone_graph_top(s_g, l) + 2) outside.push_back({l, c});
        double rim_sum = 0, rim_max = 0, low_sum = 0, low_max = 0;
        for(const auto& t : outside)
        {
            int     k;
            int32_t w[2];
            tone_graph_rim(s_g, t.first, t.second, &k, w);
            const double wa = w[0], wb = w[1], s = wa + wb;
            const double pl = (wa * s_g.l[s_g.rim_a[k]] + wb * s_g.l[s_g.rim_b[k]]) / s;
            const double pc = (wa * s_g.c[s_g.rim_a[k]] + wb * s_g.c[s_g.rim_b[k]]) / s;
            const double d = std::hypot(pl - t.first, pc - t.second), dl = t.second - tone_graph_top(s_g, t.first);
            rim_sum += d, rim_max = std::max(rim_max, d), low_sum += dl, low_max = std::max(low_max, dl);
        }
        const double q = 1.0 / TONE_GRAPH_ONE, n_out = static_cast<double>(outside.size());
        printf("the focal point of the focal rule, over the %zu targets more than 2 above the top. The closest rule: "
               "mean %.4f, worst %.4f. The target lowered to the top: mean %.4f, worst %.4f\n",
               outside.size(), rim_sum / n_out * q, rim_max * q, low_sum / n_out * q, low_max * q);
        double best = 1e30;
        int    bl = 0, bc = 0;
        for(int cf : {0, 25, 50, 100, 200})
        {
            printf("  F at c %4d (2C %.4f):", cf, cf * q);
            for(int lf = 1600; lf <= 2900; lf += 50)
            {
                bool star = true;
                for(int k = 0; k + 1 < nk && star; k++)
                    star = static_cast<int64_t>(s_g.top_l[k] - lf) * (s_g.top_c[k + 1] - cf) -
                               static_cast<int64_t>(s_g.top_c[k] - cf) * (s_g.top_l[k + 1] - lf) <
                           0;
                if(!star)
                {
                    printf("  L %.3f not star", lf * q);
                    continue;
                }
                double sum = 0, worst = 0;
                for(const auto& t : outside)
                {
                    const int64_t dl = t.first - lf, dc = t.second - cf;
                    auto          cr = [&](int k) { return dl * (s_g.top_c[k] - cf) - dc * (s_g.top_l[k] - lf); };
                    int           lo = 0, hi = nk - 1;
                    while(hi - lo > 1)
                    {
                        const int mid = (lo + hi) / 2;
                        (cr(mid) >= 0 ? lo : hi) = mid;
                    }
                    const double ca = std::fabs(static_cast<double>(cr(lo))), cb = std::fabs(static_cast<double>(cr(hi)));
                    const double pl = (cb * s_g.top_l[lo] + ca * s_g.top_l[hi]) / (ca + cb);
                    const double pc = (cb * s_g.top_c[lo] + ca * s_g.top_c[hi]) / (ca + cb);
                    const double d  = std::hypot(pl - t.first, pc - t.second);
                    sum += d, worst = std::max(worst, d);
                }
                printf("  L %.3f mean %.4f worst %.4f", lf * q, sum / n_out * q, worst * q);
                if(sum < best) best = sum, bl = lf, bc = cf;
            }
            printf("\n");
        }
        printf("lowest mean at F = l %d c %d (L %.3f, 2C %.4f): %.4f\n", bl, bc, bl * q, bc * q, best / n_out * q);
    }

    // ---- the triangles of the plain Delaunay triangulation of the tone points that the graph lacks:
    // every three tones, counter clockwise, with no tone strictly inside their circumcircle, exact.
    {
        typedef __int128 i128;
        auto orient = [&](int a, int b, int c) {
            return static_cast<int64_t>(s_g.l[b] - s_g.l[a]) * (s_g.c[c] - s_g.c[a]) -
                   static_cast<int64_t>(s_g.c[b] - s_g.c[a]) * (s_g.l[c] - s_g.l[a]);
        };
        // Above 0 when d lies inside the circle through a, b and c, counter clockwise.
        auto incircle = [&](int a, int b, int c, int d) {
            const i128 al = s_g.l[a] - s_g.l[d], ac = s_g.c[a] - s_g.c[d], bl = s_g.l[b] - s_g.l[d], bc = s_g.c[b] - s_g.c[d];
            const i128 cl = s_g.l[c] - s_g.l[d], cc = s_g.c[c] - s_g.c[d];
            const i128 a2 = al * al + ac * ac, b2 = bl * bl + bc * bc, c2 = cl * cl + cc * cc;
            return al * (bc * c2 - b2 * cc) - ac * (bl * c2 - b2 * cl) + a2 * (bl * cc - bc * cl);
        };
        int dt = 0, missing = 0, cocircular = 0;
        printf("triangles of the plain Delaunay triangulation of the %d tone points that the graph lacks:\n", N);
        for(int a = 0; a < N; a++)
            for(int b = a + 1; b < N; b++)
                for(int c = b + 1; c < N; c++)
                {
                    int64_t o = orient(a, b, c);
                    if(o == 0) continue;
                    const int p = a, q = o > 0 ? b : c, r = o > 0 ? c : b;
                    bool      empty = true, on = false;
                    for(int d = 0; d < N && empty; d++)
                    {
                        if(d == a || d == b || d == c) continue;
                        const i128 x = incircle(p, q, r, d);
                        empty        = x <= 0;
                        on           = on || x == 0;
                    }
                    if(!empty) continue;
                    dt++, cocircular += on;
                    std::array<int, 3> t = {a, b, c};
                    if(tri.count(t)) continue;
                    missing++;
                    double longest = 0;
                    for(int i = 0; i < 3; i++)
                    {
                        const int u = t[static_cast<size_t>(i)], v = t[static_cast<size_t>((i + 1) % 3)];
                        longest     = std::max(longest, std::hypot(s_g.l[u] - s_g.l[v], s_g.c[u] - s_g.c[v]));
                    }
                    // Targets of the area in it, all and from the strip up.
                    int64_t in_all = 0, in_col = 0;
                    const int l0 = std::min({s_g.l[a], s_g.l[b], s_g.l[c]}), l1 = std::max({s_g.l[a], s_g.l[b], s_g.l[c]});
                    const int c1 = std::max({s_g.c[a], s_g.c[b], s_g.c[c]});
                    for(int l = std::max(l0, 0); l <= std::min(l1, TONE_GRAPH_ONE); l++)
                        for(int cc = 0; cc <= std::min(c1, top[static_cast<size_t>(l)]); cc++)
                        {
                            const int64_t s0 = static_cast<int64_t>(s_g.l[q] - s_g.l[p]) * (cc - s_g.c[p]) - static_cast<int64_t>(s_g.c[q] - s_g.c[p]) * (l - s_g.l[p]);
                            const int64_t s1 = static_cast<int64_t>(s_g.l[r] - s_g.l[q]) * (cc - s_g.c[q]) - static_cast<int64_t>(s_g.c[r] - s_g.c[q]) * (l - s_g.l[q]);
                            const int64_t s2 = static_cast<int64_t>(s_g.l[p] - s_g.l[r]) * (cc - s_g.c[r]) - static_cast<int64_t>(s_g.c[p] - s_g.c[r]) * (l - s_g.l[r]);
                            if(s0 >= 0 && s1 >= 0 && s2 >= 0) in_all++, in_col += cc >= TONE_GRAPH_STRIP;
                        }
                    printf("  ");
                    name(a), printf(", "), name(b), printf(", "), name(c);
                    printf("\n    longest side %.4f, %lld targets of the area in it, %lld of them from the strip up%s\n",
                           longest / TONE_GRAPH_ONE, static_cast<long long>(in_all), static_cast<long long>(in_col),
                           on ? ", a fourth tone on its circle" : "");
                }
        printf("the plain Delaunay triangulation has %d triangles, %d of them with a fourth tone on the circle; the "
               "graph lacks %d and has %zu\n",
               dt, cocircular, missing, tri.size());
    }

    printf("sizes: compact %zu bytes (graph_c %zu, graph_ring %d), owner grid %zu bytes (graph_cell %zu, graph_cand "
           "%d), hydrated struct tone_graph %zu bytes\n",
           sizeof(tone_tables::graph_c) + tone_tables::graph_ring_count, sizeof(tone_tables::graph_c),
           tone_tables::graph_ring_count, sizeof(tone_tables::graph_cell) + tone_tables::graph_cand_count,
           sizeof(tone_tables::graph_cell), tone_tables::graph_cand_count, sizeof(tone_graph));
    return 0;
}
