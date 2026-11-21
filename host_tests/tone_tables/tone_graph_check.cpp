// Check of the graph of the tones (main/tone_graph.h) in main/tone_tables.inc, through the queries
// of the header on the graph that tone_graph_hydrate() builds from the file. Every reference here
// is computed from the tone points alone and shares no code with tone_graph_build.cpp:
//   owner     tone_graph_owner() against the nearest tone by brute force, the strip rule and ties
//             to the lower entry, at every lattice point of the owner grid. All of it is integer,
//             so the two agree on borders and ties too.
//   pairs     B is a neighbour of A exactly when A is one of B, and each triangle's three pairs are
//             neighbour pairs.
//   borders   per pair of tones, the length of their border inside the area, cut exactly along the
//             bisector, the strip line and the lines between neutral bands. Its pairs of non zero
//             length must be the graph's pairs.
//   raster    on the lattice of the targets the squeeze can produce, every two adjacent points of
//             different owners must be neighbours, unless a third region lies between them, and
//             each pair of the graph must show, unless its border is shorter than 2 steps.
//   triangles the triples whose regions meet at a point of the area and that are neighbours of each
//             other, from the points equidistant to them, must be the graph's triangles. third()
//             must give the triangle on each side.
//   paths     no path of the graph runs from paper to the tone of most chroma through neutral tones
//             alone.
//   toward    for targets T in the region of A, tone_graph_toward(A, T - A) against the first other
//             owner along the ray from T, found exactly. The entries may give no coloured region a
//             border on the strip line, so a graph of three tones made for it checks rays across it.
// --break K applies fault K, and the run must then fail. Exit 0 when every check passes.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "tone_graph.h"
#include "tone_tables.h"

namespace
{
typedef __int128 i128;

const int N = static_cast<int>(TONEMAP_COUNT);

tone_graph s_g;
uint16_t   s_cell[tone_tables::graph_cols * tone_tables::graph_rows];
uint8_t    s_cand[tone_tables::graph_cand_count];

bool neutral(int e)
{
    return s_g.neutral[e] != 0;
}

int paper()
{
    for(int e = 0; e < N; e++)
        if(strcmp(k_tonemap[e].pattern, TONEMAP_PAPER_PATTERN) == 0) return e;
    return 0;
}

// The squared distance from tone e to (x / k, y / k), times k^2.
i128 dist(int e, i128 x, i128 y, i128 k)
{
    const i128 dx = x - s_g.l[e] * k, dy = y - s_g.c[e] * k;
    return dx * dx + dy * dy;
}

// The owner of (x / k, y / k) by brute force, the lower entry on ties. In the strip only the
// neutral tones compete.
int brute(i128 x, i128 y, i128 k)
{
    const bool strip = y < TONE_GRAPH_STRIP * k;
    int        best  = -1;
    i128       bd    = 0;
    for(int e = 0; e < N; e++)
    {
        if(strip && !neutral(e)) continue;
        const i128 d = dist(e, x, y, k);
        if(best < 0 || d < bd) best = e, bd = d;
    }
    return best;
}

// ---- the area, a x + b y <= c: from the squeeze's four panel edge lines, as tone_graph.h states.
struct hp
{
    int64_t a, b, c;
};
std::vector<hp> s_area;

void make_area()
{
    using namespace tone_fx_squeeze_internal;
    const int16_t* k = tone_tables::squeeze_k.data();
    s_area           = {{-1, 0, -tone_graph_l12(k[K_LB])}, {1, 0, TONE_GRAPH_ONE}, {0, -1, 0}};
    const int64_t pm[4] = {k[K_PM0], k[K_PM1], k[K_PM2], k[K_PM3]};
    const int64_t pb[4] = {0, k[K_PB1], k[K_PB2], k[K_PB3]};
    for(int i = 0; i < 4; i++) s_area.push_back({-8 * pm[i], 131072, pm[i] * (pm[i] >= 0 ? 3 : -4) + 32768 * pb[i] + 65536});
}

bool in_area(i128 x, i128 y, i128 k)
{
    for(const hp& h : s_area)
        if(h.a * x + h.b * y > h.c * k) return false;
    return true;
}

// The highest c the squeeze can give at l: the most of pan_edge() over the L that round to l.
std::vector<int> s_top;

void make_top()
{
    using namespace tone_fx_squeeze_internal;
    const int16_t* k = tone_tables::squeeze_k.data();
    s_top.assign(TONE_GRAPH_ONE + 1, -1);
    for(int L = 0; L <= 32767; L++)
    {
        const int L2 = std::max(L, static_cast<int>(k[K_LB]));
        const int l  = tone_graph_l12(L2);
        s_top[static_cast<size_t>(l)] = std::max(s_top[static_cast<size_t>(l)], tone_graph_c12(pan_edge(k, static_cast<int16_t>(L))));
    }
}

// ---- the exact border of two tones: the stretch of a line where both own, as an interval of t
struct span
{
    bool empty = false, lo_set = false, hi_set = false;
    i128 lo_n = 0, lo_d = 1, hi_n = 0, hi_d = 1;

    // k t <= r
    void cut(i128 k, i128 r)
    {
        if(k == 0)
        {
            if(r < 0) empty = true;
            return;
        }
        i128 n = r, d = k;
        if(d < 0) n = -n, d = -d;
        if(k > 0)
        {
            if(!hi_set || n * hi_d < hi_n * d) hi_n = n, hi_d = d, hi_set = true;
        }
        else if(!lo_set || n * lo_d > lo_n * d)
            lo_n = n, lo_d = d, lo_set = true;
    }
    double length() const
    {
        if(empty || !lo_set || !hi_set || hi_n * lo_d <= lo_n * hi_d) return 0;
        return static_cast<double>(hi_n) / static_cast<double>(hi_d) - static_cast<double>(lo_n) / static_cast<double>(lo_d);
    }
};

// On the line P(t) = (s + t v) / 2, the half plane a x + b y <= c.
void cut_line(span& sp, i128 sx, i128 sy, i128 vx, i128 vy, i128 a, i128 b, i128 c)
{
    sp.cut(a * vx + b * vy, 2 * c - a * sx - b * sy);
}

// e as near as f, or nearer: 2 (f - e).P <= |f|^2 - |e|^2.
void near_cut(span& sp, i128 sx, i128 sy, i128 vx, i128 vy, int e, int f)
{
    const i128 a = 2 * (s_g.l[f] - s_g.l[e]), b = 2 * (s_g.c[f] - s_g.c[e]);
    const i128 c = static_cast<i128>(s_g.l[f]) * s_g.l[f] + static_cast<i128>(s_g.c[f]) * s_g.c[f] -
                   static_cast<i128>(s_g.l[e]) * s_g.l[e] - static_cast<i128>(s_g.c[e]) * s_g.c[e];
    cut_line(sp, sx, sy, vx, vy, a, b, c);
}

bool same_point(int e, int f)
{
    return s_g.l[e] == s_g.l[f] && s_g.c[e] == s_g.c[f];
}

// A tone at the point of a lower one owns nothing.
bool shadowed(int e)
{
    for(int f = 0; f < e; f++)
        if(same_point(e, f)) return true;
    return false;
}

double border(int e, int f)
{
    if(shadowed(e) || shadowed(f)) return 0;
    double len = 0;
    // Above the strip, along the bisector: P = (Pe + Pf + t v) / 2 with v at right angles to f - e.
    {
        const i128 sx = s_g.l[e] + s_g.l[f], sy = s_g.c[e] + s_g.c[f];
        const i128 vx = -(s_g.c[f] - s_g.c[e]), vy = s_g.l[f] - s_g.l[e];
        span       sp;
        for(const hp& h : s_area) cut_line(sp, sx, sy, vx, vy, h.a, h.b, h.c);
        cut_line(sp, sx, sy, vx, vy, 0, -1, -TONE_GRAPH_STRIP);
        for(int g = 0; g < N; g++)
            if(g != e && g != f && !shadowed(g)) near_cut(sp, sx, sy, vx, vy, e, g);
        len += sp.length() * std::sqrt(static_cast<double>(vx * vx + vy * vy)) / 2;
    }
    // In the strip, between two neutral bands: P = (xe + xf, 2 t) / 2.
    if(neutral(e) && neutral(f))
    {
        const i128 sx = s_g.l[e] + s_g.l[f];
        span       sp;
        for(const hp& h : s_area) cut_line(sp, sx, 0, 0, 2, h.a, h.b, h.c);
        cut_line(sp, sx, 0, 0, 2, 0, -1, 0);
        cut_line(sp, sx, 0, 0, 2, 0, 1, TONE_GRAPH_STRIP);
        for(int g = 0; g < N; g++)
            if(g != e && g != f && neutral(g) && !shadowed(g)) near_cut(sp, sx, 0, 0, 2, e, g);
        len += sp.length();
    }
    // On the strip line, u above and n below: P = (2 t, 2 STRIP) / 2.
    for(int pass = 0; pass < 2; pass++)
    {
        const int u = pass ? f : e, n = pass ? e : f;
        if(!neutral(n)) continue;
        span sp;
        for(const hp& h : s_area) cut_line(sp, 0, 2 * TONE_GRAPH_STRIP, 2, 0, h.a, h.b, h.c);
        for(int g = 0; g < N; g++)
        {
            if(shadowed(g)) continue;
            if(g != u) near_cut(sp, 0, 2 * TONE_GRAPH_STRIP, 2, 0, u, g);
            if(g != n && neutral(g))
            {
                // n's band: n as near in l as g.
                const i128 a = 2 * (s_g.l[g] - s_g.l[n]);
                const i128 c = static_cast<i128>(s_g.l[g]) * s_g.l[g] - static_cast<i128>(s_g.l[n]) * s_g.l[n];
                cut_line(sp, 0, 2 * TONE_GRAPH_STRIP, 2, 0, a, 0, c);
            }
        }
        len += sp.length();
    }
    return len;
}

// ---- the graph as hydrated
bool has(int e, int f)
{
    for(unsigned k = s_g.at[e]; k < s_g.at[e + 1]; k++)
        if(s_g.nb[k] == f) return true;
    return false;
}

std::set<std::array<int, 3>> graph_triangles()
{
    std::set<std::array<int, 3>> t;
    for(int e = 0; e < N; e++)
    {
        const unsigned k0 = s_g.at[e], deg = s_g.at[e + 1] - k0;
        for(unsigned j = 0; j < deg; j++)
            if(s_g.fl[k0 + j] & TONE_GRAPH_TRI)
            {
                std::array<int, 3> x = {e, s_g.nb[k0 + j], s_g.nb[k0 + (j + 1) % deg]};
                std::sort(x.begin(), x.end());
                t.insert(x);
            }
    }
    return t;
}

// Applies fault k to the hydrated graph.
void fault(int k)
{
    switch(k)
    {
    case 1:
        // One cell of the owner grid loses its last candidate.
        for(int i = 0; i < tone_tables::graph_cols * tone_tables::graph_rows; i++)
            if((s_cell[i] & 15) >= 4)
            {
                s_cell[i]--;
                break;
            }
        break;
    case 2:
        // Entry 10 forgets its first neighbour, which keeps it.
        s_g.nb[s_g.at[10]] = s_g.nb[s_g.at[10] + 1];
        break;
    case 3:
    {
        // Paper and the tone of most chroma become neighbours: each replaces the last neighbour of
        // the other.
        int top = 0;
        for(int e = 1; e < N; e++)
            if(s_g.c[e] > s_g.c[top]) top = e;
        s_g.nb[s_g.at[paper() + 1] - 1] = static_cast<uint8_t>(top);
        s_g.nb[s_g.at[top + 1] - 1]     = static_cast<uint8_t>(paper());
        break;
    }
    case 4:
    {
        // Entry 30 and its first neighbour stop being neighbours on both sides.
        const int e = 30, f = s_g.nb[s_g.at[e]];
        for(int x : {e, f})
        {
            const int y = x == e ? f : e;
            unsigned  w = s_g.at[x];
            for(unsigned r = s_g.at[x]; r < s_g.at[x + 1]; r++)
                if(s_g.nb[r] != y) s_g.nb[w] = s_g.nb[r], s_g.fl[w] = s_g.fl[r], w++;
            for(; w < s_g.at[x + 1]; w++) s_g.nb[w] = s_g.nb[w - 1], s_g.fl[w] = 0;
        }
        break;
    }
    case 5:
        // The first triangle flag of entry 40 is lost.
        for(unsigned r = s_g.at[40]; r < s_g.at[41]; r++)
            if(s_g.fl[r] & TONE_GRAPH_TRI)
            {
                s_g.fl[r] = 0;
                break;
            }
        break;
    case 6:
        // In the three tone graph of the strip line check, main() drops the second neutral tone from
        // the pink's ring.
        break;
    }
}

struct tow_count
{
    int n = 0, bad = 0, out = 0, strip = 0, behind = 0;
};

// tone_graph_toward(a, T - a) for the target T = (l, c) of a's region, against the first region the
// ray from a through T enters past T, found exactly.
void toward_one(int a, int l, int c, tow_count& t)
{
    const int dl = l - s_g.l[a], dc = c - s_g.c[a];
    if(dl == 0 && dc == 0) return;
    t.n++;
    // The owner of T + u (dl, dc) for u = n / m, or -1 outside the area.
    auto at = [&](i128 n, i128 m) {
        const i128 x = static_cast<i128>(l) * m + n * dl, y = static_cast<i128>(c) * m + n * dc;
        return in_area(x, y, m) ? brute(x, y, m) : -1;
    };
    // March from T in steps of about one until a leaves.
    const double d = std::sqrt(static_cast<double>(dl) * dl + static_cast<double>(dc) * dc);
    const int    K = static_cast<int>(std::ceil(d));
    int          k = 1, o = a;
    for(; o == a; k++) o = at(k, K);
    k--;
    if(o < 0)
    {
        t.out++;
        return;
    }
    // Over that step and the next the owner changes only where two tones are equally near or the ray
    // meets the strip line. Between two such points one tone owns the whole stretch, so the owner at
    // a point of each stretch, in order, gives the first region past a exactly, also when a leaves at
    // the end of the step, a point that a tie may give away.
    std::vector<std::pair<i128, i128>> u = {{k - 1, K}, {k + 1, K}};
    auto                               keep = [&](i128 n, i128 m) {
        if(m == 0) return;
        if(m < 0) n = -n, m = -m;
        if(n * K > (k - 1) * m && n * K < (k + 1) * m) u.push_back({n, m});
    };
    for(int f = 0; f < N; f++)
        for(int h = f + 1; h < N; h++)
            keep(dist(h, l, c, 1) - dist(f, l, c, 1),
                 2 * (static_cast<i128>(dl) * (s_g.l[h] - s_g.l[f]) + static_cast<i128>(dc) * (s_g.c[h] - s_g.c[f])));
    keep(TONE_GRAPH_STRIP - c, dc);
    std::sort(u.begin(), u.end(), [](const std::pair<i128, i128>& p, const std::pair<i128, i128>& q) {
        return p.first * q.second < q.first * p.second;
    });
    int  first = a;
    bool strip = false;
    for(size_t i = 0; i + 1 < u.size() && first == a; i++)
    {
        if(u[i].first * u[i + 1].second == u[i + 1].first * u[i].second) continue;
        const i128 n = u[i].first + u[i + 1].first, m = u[i].second + u[i + 1].second;
        first        = at(n, m);
        strip        = static_cast<i128>(c) * m + n * dc < TONE_GRAPH_STRIP * m;
    }
    if(first < 0)
    {
        t.out++;
        return;
    }
    t.strip += strip;
    const int64_t dot  = static_cast<int64_t>(s_g.l[first] - s_g.l[a]) * dl + static_cast<int64_t>(s_g.c[first] - s_g.c[a]) * dc;
    const int     want = dot > 0 ? first : -1;
    t.behind += dot <= 0;
    const int got = tone_graph_toward(s_g, static_cast<unsigned>(a), dl, dc);
    if(got != want)
    {
        if(t.bad < 5) printf("FAIL toward from %d at %d %d to %d %d: %d, the march meets %d\n", a, s_g.l[a], s_g.c[a], l, c, got, want);
        t.bad++;
    }
}
} // namespace

int main(int argc, char** argv)
{
    const int brk = argc == 3 && strcmp(argv[1], "--break") == 0 ? atoi(argv[2]) : 0;
    std::copy(tone_tables::graph_cell.begin(), tone_tables::graph_cell.end(), s_cell);
    std::copy(tone_tables::graph_cand.begin(), tone_tables::graph_cand.end(), s_cand);
    if(!tone_graph_hydrate(&s_g, tone_tables::qL.data(), tone_tables::graph_c.data(), tone_tables::neutral.data(),
                           tone_tables::neutral_count, tone_tables::graph_ring.data(), tone_tables::graph_ring_count,
                           s_cell, s_cand, tone_tables::graph_rows))
    {
        printf("FAIL tone_graph_hydrate refuses the rings\n");
        return 1;
    }
    fault(brk);
    make_area();
    make_top();
    int bad = 0;

    // ---- owner: every lattice point of the grid, and two rows below c = 0
    const int H   = s_g.rows << TONE_GRAPH_CELL_SHIFT;
    int       own_bad = 0, most = 0;
    std::vector<uint8_t> img(static_cast<size_t>((TONE_GRAPH_ONE + 1) * H));
    for(int l = 0; l <= TONE_GRAPH_ONE; l++)
        for(int c = -2; c < H; c++)
        {
            const int want = brute(l, c, 1);
            const int got  = static_cast<int>(tone_graph_owner(s_g, l, c));
            if(c >= 0) img[static_cast<size_t>(l * H + c)] = static_cast<uint8_t>(want);
            if(c >= 0 && c <= s_top[static_cast<size_t>(l)])
                most = std::max(most, static_cast<int>(s_g.cell[std::min(l >> TONE_GRAPH_CELL_SHIFT, TONE_GRAPH_COLS - 1) * s_g.rows +
                                                                (c >> TONE_GRAPH_CELL_SHIFT)] &
                                                         15));
            if(got != want)
            {
                if(own_bad < 5) printf("FAIL owner of %d %d: %d, nearest %d\n", l, c, got, want);
                own_bad++;
            }
        }
    bad += own_bad;
    int64_t   pts = 0;
    unsigned  sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for(int rep = 0; rep < 4; rep++)
        for(int l = 0; l <= TONE_GRAPH_ONE; l++)
            for(int c = TONE_GRAPH_STRIP; c <= s_top[static_cast<size_t>(l)]; c++) sink += tone_graph_owner(s_g, l, c), pts++;
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / pts;
    printf("owner: %d of %d lattice points differ from the nearest tone, at most %d candidates at a target, %.1f ms "
           "per million queries on this host (%u)\n",
           own_bad, (TONE_GRAPH_ONE + 1) * (H + 2), most, ns, sink & 1);

    // ---- pairs
    int sym_bad = 0;
    for(int e = 0; e < N; e++)
        for(unsigned k = s_g.at[e]; k < s_g.at[e + 1]; k++)
            if(!has(s_g.nb[k], e) || s_g.nb[k] == e)
            {
                if(sym_bad < 5) printf("FAIL pairs: %d lists %u, which does not list it\n", e, s_g.nb[k]);
                sym_bad++;
            }
    const std::set<std::array<int, 3>> gt = graph_triangles();
    for(const auto& t : gt)
        if(!has(t[0], t[1]) || !has(t[1], t[2]) || !has(t[0], t[2]))
        {
            if(sym_bad < 5) printf("FAIL pairs: triangle %d %d %d has a pair that are not neighbours\n", t[0], t[1], t[2]);
            sym_bad++;
        }
    bad += sym_bad;
    printf("pairs: %d faults\n", sym_bad);

    // ---- borders
    std::vector<double> len(static_cast<size_t>(N * N), 0);
    int                 bord_bad = 0, n_pairs = 0;
    for(int e = 0; e < N; e++)
        for(int f = e + 1; f < N; f++)
        {
            const double b = border(e, f);
            len[static_cast<size_t>(e * N + f)] = len[static_cast<size_t>(f * N + e)] = b;
            const bool nb = has(e, f) && has(f, e);
            n_pairs += nb;
            if((b > 0) != nb)
            {
                if(bord_bad < 5) printf("FAIL borders: %d and %d share %.4f steps, the graph says %s\n", e, f, b, nb ? "neighbours" : "not");
                bord_bad++;
            }
        }
    bad += bord_bad;
    printf("borders: %d neighbour pairs, %d faults\n", n_pairs, bord_bad);

    // ---- raster
    int                           ras_bad = 0, ras_thin = 0, ras_short = 0;
    std::set<std::pair<int, int>> seen;
    for(int l = 0; l <= TONE_GRAPH_ONE; l++)
        for(int c = 0; c <= s_top[static_cast<size_t>(l)]; c++)
        {
            const int o = img[static_cast<size_t>(l * H + c)];
            for(int dir = 0; dir < 2; dir++)
            {
                const int l2 = l + (dir == 0), c2 = c + (dir == 1);
                if(l2 > TONE_GRAPH_ONE || c2 > s_top[static_cast<size_t>(l2)]) continue;
                const int o2 = img[static_cast<size_t>(l2 * H + c2)];
                if(o2 == o) continue;
                seen.insert({std::min(o, o2), std::max(o, o2)});
                if(has(o, o2)) continue;
                // A third region between the two points explains it.
                bool third = false;
                for(int s = 1; s < 256 && !third; s++)
                {
                    const int w = brute(256 * l + s * (l2 - l), 256 * c + s * (c2 - c), 256);
                    third       = w != o && w != o2;
                }
                if(third)
                {
                    ras_thin++;
                    continue;
                }
                if(ras_bad < 5) printf("FAIL raster: %d and %d meet between %d %d and %d %d\n", o, o2, l, c, l2, c2);
                ras_bad++;
            }
        }
    for(int e = 0; e < N; e++)
        for(unsigned k = s_g.at[e]; k < s_g.at[e + 1]; k++)
        {
            const int f = s_g.nb[k];
            if(e > f || seen.count({e, f})) continue;
            if(len[static_cast<size_t>(e * N + f)] < 2)
            {
                ras_short++;
                continue;
            }
            if(ras_bad < 5) printf("FAIL raster: %d and %d share %.1f steps, no adjacent points show it\n", e, f, len[static_cast<size_t>(e * N + f)]);
            ras_bad++;
        }
    bad += ras_bad;
    printf("raster: %zu pairs meet on the lattice, %d of them across a thin third region, %d graph pairs too short to "
           "show, %d faults\n",
           seen.size(), ras_thin, ras_short, ras_bad);

    // ---- triangles
    std::set<std::array<int, 3>> meet;
    auto add = [&](int a, int b, int c) {
        if(a == b || b == c || a == c) return;
        std::array<int, 3> t = {a, b, c};
        std::sort(t.begin(), t.end());
        if(len[static_cast<size_t>(t[0] * N + t[1])] > 0 && len[static_cast<size_t>(t[1] * N + t[2])] > 0 &&
           len[static_cast<size_t>(t[0] * N + t[2])] > 0)
            meet.insert(t);
    };
    // The tones nearest (x / k, y / k) above the strip, and the neutral ones nearest below it.
    auto nearest = [&](i128 x, i128 y, i128 k, bool band, std::vector<int>& out) {
        out.clear();
        i128 bd = -1;
        for(int g = 0; g < N; g++)
        {
            if((band && !neutral(g)) || shadowed(g)) continue;
            const i128 d = dist(g, x, y, k);
            if(bd < 0 || d < bd) bd = d, out.clear();
            if(d == bd) out.push_back(g);
        }
    };
    std::vector<int> up, dn;
    for(int a = 0; a < N; a++)
        for(int b = a + 1; b < N; b++)
        {
            if(shadowed(a) || shadowed(b)) continue;
            const i128 ba = 2 * (s_g.l[b] - s_g.l[a]), bb = 2 * (s_g.c[b] - s_g.c[a]);
            const i128 bc = static_cast<i128>(s_g.l[b]) * s_g.l[b] + static_cast<i128>(s_g.c[b]) * s_g.c[b] -
                            static_cast<i128>(s_g.l[a]) * s_g.l[a] - static_cast<i128>(s_g.c[a]) * s_g.c[a];
            // The bisector of a and b meets the strip line at x = (bc - bb STRIP) / ba.
            if(ba != 0)
            {
                i128 x = bc - bb * TONE_GRAPH_STRIP, k = ba;
                if(k < 0) x = -x, k = -k;
                if(in_area(x, TONE_GRAPH_STRIP * k, k))
                {
                    nearest(x, TONE_GRAPH_STRIP * k, k, false, up);
                    if(std::find(up.begin(), up.end(), a) != up.end() && std::find(up.begin(), up.end(), b) != up.end())
                    {
                        nearest(x, TONE_GRAPH_STRIP * k, k, true, dn);
                        for(int n : dn) add(a, b, n);
                    }
                }
            }
            // Two neutral bands meet the strip line at their midpoint.
            if(neutral(a) && neutral(b) && in_area(s_g.l[a] + s_g.l[b], 2 * TONE_GRAPH_STRIP, 2))
            {
                nearest(s_g.l[a] + s_g.l[b], 2 * TONE_GRAPH_STRIP, 2, true, dn);
                if(std::find(dn.begin(), dn.end(), a) != dn.end() && std::find(dn.begin(), dn.end(), b) != dn.end())
                {
                    nearest(s_g.l[a] + s_g.l[b], 2 * TONE_GRAPH_STRIP, 2, false, up);
                    for(int u : up) add(a, b, u);
                }
            }
            for(int c = b + 1; c < N; c++)
            {
                if(shadowed(c)) continue;
                const i128 ca = 2 * (s_g.l[c] - s_g.l[a]), cb = 2 * (s_g.c[c] - s_g.c[a]);
                const i128 cc = static_cast<i128>(s_g.l[c]) * s_g.l[c] + static_cast<i128>(s_g.c[c]) * s_g.c[c] -
                                static_cast<i128>(s_g.l[a]) * s_g.l[a] - static_cast<i128>(s_g.c[a]) * s_g.c[a];
                i128 k = ba * cb - ca * bb;
                if(k == 0) continue;
                i128 x = bc * cb - cc * bb, y = ba * cc - ca * bc;
                if(k < 0) x = -x, y = -y, k = -k;
                if(y < TONE_GRAPH_STRIP * k || !in_area(x, y, k)) continue;
                nearest(x, y, k, false, up);
                if(std::find(up.begin(), up.end(), a) != up.end() && std::find(up.begin(), up.end(), b) != up.end() &&
                   std::find(up.begin(), up.end(), c) != up.end())
                    add(a, b, c);
            }
        }
    int tri_bad = 0;
    for(const auto& t : meet)
        if(!gt.count(t))
        {
            if(tri_bad < 5) printf("FAIL triangles: %d %d %d meet in the area, the graph lacks it\n", t[0], t[1], t[2]);
            tri_bad++;
        }
    for(const auto& t : gt)
        if(!meet.count(t))
        {
            if(tri_bad < 5) printf("FAIL triangles: the graph has %d %d %d, whose regions do not meet\n", t[0], t[1], t[2]);
            tri_bad++;
        }
    // third() on each side of each pair against the triangles that meet.
    int third_bad = 0, inverted = 0;
    for(int a = 0; a < N; a++)
        for(unsigned k = s_g.at[a]; k < s_g.at[a + 1]; k++)
        {
            const int b = s_g.nb[k];
            for(int side : {1, -1})
            {
                std::vector<int> want;
                for(const auto& t : meet)
                    if(std::find(t.begin(), t.end(), a) != t.end() && std::find(t.begin(), t.end(), b) != t.end())
                    {
                        const int     c  = t[0] != a && t[0] != b ? t[0] : t[1] != a && t[1] != b ? t[1] : t[2];
                        const int64_t cr = static_cast<int64_t>(s_g.l[b] - s_g.l[a]) * (s_g.c[c] - s_g.c[a]) -
                                           static_cast<int64_t>(s_g.c[b] - s_g.c[a]) * (s_g.l[c] - s_g.l[a]);
                        if((side > 0 && cr > 0) || (side < 0 && cr < 0)) want.push_back(c);
                    }
                inverted += want.size() > 1;
                const int got = tone_graph_third(s_g, static_cast<unsigned>(a), static_cast<unsigned>(b), side);
                const bool ok = want.empty() ? got == -1 : std::find(want.begin(), want.end(), got) != want.end();
                if(!ok)
                {
                    if(third_bad < 5)
                        printf("FAIL third of %d %d on side %d: %d, want %d\n", a, b, side, got, want.empty() ? -1 : want[0]);
                    third_bad++;
                }
            }
        }
    bad += tri_bad + third_bad;
    printf("triangles: %zu meet in the area, the graph has %zu, %d faults, third() %d faults, %d sides with two "
           "triangles\n",
           meet.size(), gt.size(), tri_bad, third_bad, inverted);

    // ---- paths
    int top = 0;
    for(int e = 1; e < N; e++)
        if(s_g.c[e] > s_g.c[top]) top = e;
    std::vector<bool> reach(static_cast<size_t>(N), false);
    std::vector<int>  q = {paper()};
    reach[static_cast<size_t>(paper())] = true;
    while(!q.empty())
    {
        const int e = q.back();
        q.pop_back();
        for(unsigned k = s_g.at[e]; k < s_g.at[e + 1]; k++)
        {
            const int f = s_g.nb[k];
            if(reach[static_cast<size_t>(f)] || (!neutral(f) && f != top)) continue;
            reach[static_cast<size_t>(f)] = true;
            if(f != top) q.push_back(f);
        }
    }
    const bool path_bad = reach[static_cast<size_t>(top)];
    bad += path_bad;
    printf("paths: entry %d, the most chroma, %s from paper through neutral tones alone\n", top,
           path_bad ? "FAIL: reachable" : "not reachable");

    // ---- toward
    tow_count tw;
    for(int l = 3; l <= TONE_GRAPH_ONE; l += 13)
        for(int c = TONE_GRAPH_STRIP; c <= s_top[static_cast<size_t>(l)]; c += 5) toward_one(img[static_cast<size_t>(l * H + c)], l, c, tw);
    bad += tw.bad;
    printf("toward: %d targets, %d leave the area first, %d enter a neutral band across the strip line, %d meet a "
           "neutral behind the tone, %d faults\n",
           tw.n, tw.out, tw.strip, tw.behind, tw.bad);

    // A graph of three tones whose pink reaches the strip line takes rays across it: neutral tones at
    // l 1000 and 3000, the pink at l 2100 and c 20 between them, and every other entry at the first
    // neutral's point, where it owns nothing. It must give both kinds of result of that branch.
    {
        int16_t qs[TONEMAP_COUNT], cs[TONEMAP_COUNT];
        for(int e = 0; e < N; e++) qs[e] = 8 * 1000, cs[e] = 0;
        qs[1] = 8 * 3000, qs[2] = 8 * 2100, cs[2] = 20;
        const uint8_t        nt[2] = {0, 1};
        std::vector<uint8_t> rg    = {1, 2, TONE_GRAPH_GAP, TONE_GRAPH_END, 2, 0, TONE_GRAPH_GAP, TONE_GRAPH_END,
                                      0, 1, TONE_GRAPH_GAP, TONE_GRAPH_END};
        if(brk == 6) rg[9] = TONE_GRAPH_GAP, rg[10] = TONE_GRAPH_END, rg.pop_back();
        rg.resize(rg.size() + N - 3, TONE_GRAPH_END);
        if(!tone_graph_hydrate(&s_g, qs, cs, nt, 2, rg.data(), static_cast<int>(rg.size()), s_cell, s_cand, 1))
        {
            printf("FAIL tone_graph_hydrate refuses the three tone graph\n");
            return 1;
        }
        tow_count ts;
        for(int l = 1500; l <= 2700; l += 7)
            for(int c = TONE_GRAPH_STRIP; c <= 60; c++)
                if(brute(l, c, 1) == 2) toward_one(2, l, c, ts);
        bad += ts.bad + (ts.strip == 0) + (ts.behind == 0);
        printf("toward across the strip line: %d targets, %d enter a neutral band across it, %d meet a neutral "
               "behind the tone, %d faults\n",
               ts.n, ts.strip, ts.behind, ts.bad);
    }

    printf("tone_graph_check%s: %s\n", brk ? " with a fault" : "", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
