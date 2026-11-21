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
//   toward    for targets T in the region of A, tone_graph_toward(A, T - A) against the neighbour
//             at the least angle from T - A under 90 degrees, found exactly. A result whose angle
//             is within the rounding of the unit vectors of the least passes.
//   top       tone_graph_top() at every l against the highest point of the triangles, found exactly:
//             at most that point rounded down and at most 2 below it, or 0 where no triangle
//             reaches it.
//   pick      tone_near_pick() of tone_photo.h at every lattice point of the area from the strip up,
//             with the linear weights and with eases of the solid and the line step, by the focal
//             rule and by the closest rule (pick_run()). Every two entries of a mix with weight are
//             neighbours. With the linear weights a target in a triangle is met within PICK_REACH. A
//             target in no triangle takes the point where its segment to the focal point crosses the
//             top, found in double over every edge of the top, and a target on the top takes the
//             mix of its triangle; with the closest rule it takes the nearest point of the triangles
//             and pairs, found by brute force. With an ease the mix moves from the linear mix by at
//             most the reach of the steps, is linear beyond it and solid inside the solid t. With the
//             focal rule no mix jumps between adjacent targets; with the closest rule the jumps are
//             reported, not checked.
// The owner check runs in parts of l and the pick configurations each on a thread of their own, and
// each part keeps its report until all are done, so the output is that of one thread.
// --break K applies fault K, and the run must then fail. Exit 0 when every check passes.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "tone_graph.h"
#include "tone_photo.h"
#include "tone_tables.h"

namespace
{
typedef __int128 i128;

const int N = static_cast<int>(TONEMAP_COUNT);
// 0.002 and 0.01 of the plane.
const double PICK_REACH = 0.002 * TONE_GRAPH_ONE;
const double PICK_JUMP  = 0.01 * TONE_GRAPH_ONE;

tone_graph s_g;
uint16_t   s_cell[tone_tables::graph_cols * tone_tables::graph_rows];
uint8_t    s_cand[tone_tables::graph_cand_count];

// The queries of the graph as tone_near_pick() asks them, as in tones.cpp.
struct near_graph
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

// printf into the report of a thread.
__attribute__((format(printf, 2, 3))) void say(std::string& out, const char* fmt, ...)
{
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    out += line;
}

// The threads of the host.
int host_threads()
{
    return std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
}

// f(lo, hi, k) for the parts k of 0 to n, each on a thread, the parts in order of lo.
template <class F> void run_parts(int n, int parts, F f)
{
    std::vector<std::thread> th;
    for(int k = 0; k < parts; k++)
        th.emplace_back(f, static_cast<int>(static_cast<int64_t>(n) * k / parts),
                        static_cast<int>(static_cast<int64_t>(n) * (k + 1) / parts), k);
    for(std::thread& t : th) t.join();
}

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
        // The unit vectors of the first two neighbours of entry 30 trade places.
        std::swap(s_g.ul[s_g.at[30]], s_g.ul[s_g.at[30] + 1]);
        std::swap(s_g.uc[s_g.at[30]], s_g.uc[s_g.at[30] + 1]);
        break;
    case 7:
        // A corner in the middle of the top of the triangles rises by 0.01 of the plane.
        s_g.top_c[s_g.top_n / 2] = static_cast<int16_t>(s_g.top_c[s_g.top_n / 2] + 41);
        break;
    case 8:
    {
        // The rim loses the pair that reaches the most c.
        int top = 0;
        for(int i = 1; i < s_g.rim_n; i++)
            if(std::max(s_g.c[s_g.rim_a[i]], s_g.c[s_g.rim_b[i]]) > std::max(s_g.c[s_g.rim_a[top]], s_g.c[s_g.rim_b[top]])) top = i;
        for(int i = top; i + 1 < s_g.rim_n; i++)
        {
            s_g.rim_a[i] = s_g.rim_a[i + 1], s_g.rim_b[i] = s_g.rim_b[i + 1], s_g.rim_t[i] = s_g.rim_t[i + 1];
            s_g.rim_ul[i] = s_g.rim_ul[i + 1], s_g.rim_uc[i] = s_g.rim_uc[i + 1];
            s_g.rim_lo[i] = s_g.rim_lo[i + 1], s_g.rim_hi[i] = s_g.rim_hi[i + 1];
        }
        s_g.rim_n--;
        break;
    }
    case 9:
        // The tone at the middle corner of the top is taken for the one at the next corner.
        s_g.top_e[s_g.top_n / 2] = s_g.top_e[s_g.top_n / 2 + 1];
        break;
    }
}

struct tow_count
{
    int n = 0, bad = 0, none = 0, tie = 0;
};

// The cosine of the angle between neighbour f of a and (dl, dc), for the report of a near tie.
double cosine(int a, int f, int dl, int dc)
{
    const double ul = s_g.l[f] - s_g.l[a], uc = s_g.c[f] - s_g.c[a];
    return (ul * dl + uc * dc) / std::sqrt((ul * ul + uc * uc) * (static_cast<double>(dl) * dl + static_cast<double>(dc) * dc));
}

// tone_graph_toward(a, (dl, dc)) against the neighbour at the least angle from (dl, dc), found exactly:
// the largest dot^2 / |f - a|^2 of the neighbours f with dot = (f - a).(dl, dc) above 0. A result
// whose cosine lies within 1e-4 of the best passes as a near tie, also -1 against a best whose
// cosine is below 1e-4, and the reverse.
void toward_one(int a, int dl, int dc, tow_count& t)
{
    if(dl == 0 && dc == 0) return;
    t.n++;
    int  want = -1;
    i128 bd = 0, bn = 1;
    for(unsigned k = s_g.at[a]; k < s_g.at[a + 1]; k++)
    {
        const int  f  = s_g.nb[k];
        const i128 ul = s_g.l[f] - s_g.l[a], uc = s_g.c[f] - s_g.c[a];
        const i128 d = ul * dl + uc * dc, n = ul * ul + uc * uc;
        if(d > 0 && (want < 0 || d * d * bn > bd * bd * n)) want = f, bd = d, bn = n;
    }
    t.none += want < 0;
    const int got = tone_graph_toward(s_g, static_cast<unsigned>(a), dl, dc);
    if(got == want) return;
    const double cw = want < 0 ? 0 : cosine(a, want, dl, dc), cg = got < 0 ? 0 : cosine(a, got, dl, dc);
    if(std::fabs(cg - cw) <= 1e-4)
    {
        t.tie++;
        return;
    }
    if(t.bad < 5)
        printf("FAIL toward from %d at %d %d along %d %d: %d at cosine %.6f, the least angle is %d at %.6f\n", a, s_g.l[a],
               s_g.c[a], dl, dc, got, cg, want, cw);
    t.bad++;
}

// The highest c of the triangles at l, times den, or -1 where no triangle reaches l.
struct top_at
{
    i128 num = -1, den = 1;
};

top_at exact_top(const std::set<std::array<int, 3>>& tris, int l)
{
    top_at best;
    for(const auto& t : tris)
        for(int i = 0; i < 3; i++)
        {
            int e = t[static_cast<size_t>(i)], f = t[static_cast<size_t>((i + 1) % 3)];
            if(s_g.l[e] > s_g.l[f]) std::swap(e, f);
            if(l < s_g.l[e] || l > s_g.l[f]) continue;
            top_at v;
            if(s_g.l[e] == s_g.l[f]) v.num = std::max(s_g.c[e], s_g.c[f]);
            else
            {
                v.den = s_g.l[f] - s_g.l[e];
                v.num = static_cast<i128>(s_g.c[e]) * v.den + static_cast<i128>(s_g.c[f] - s_g.c[e]) * (l - s_g.l[e]);
            }
            if(best.num < 0 || v.num * best.den > best.num * v.den) best = v;
        }
    return best;
}

// Whether triangle t holds (l, c), its edges included.
bool holds(const std::array<int, 3>& t, int l, int c)
{
    int pos = 0, neg = 0;
    for(int i = 0; i < 3; i++)
    {
        const int     p = t[static_cast<size_t>(i)], q = t[static_cast<size_t>((i + 1) % 3)];
        const int64_t x = static_cast<int64_t>(s_g.l[q] - s_g.l[p]) * (c - s_g.c[p]) -
                          static_cast<int64_t>(s_g.c[q] - s_g.c[p]) * (l - s_g.l[p]);
        pos += x > 0, neg += x < 0;
    }
    return !(pos && neg);
}

// The point a mix reaches.
void point_of(const tone_near_mix& m, double* ml, double* mc)
{
    const double s = m.w[0] + m.w[1] + m.w[2];
    *ml = *mc = 0;
    for(int i = 0; i < 3; i++) *ml += m.w[i] / s * s_g.l[m.v[i]], *mc += m.w[i] / s * s_g.c[m.v[i]];
}

// The distance from (l, c) to the pair e f.
double seg_dist(int e, int f, double l, double c)
{
    const double vl = s_g.l[f] - s_g.l[e], vc = s_g.c[f] - s_g.c[e], el = l - s_g.l[e], ec = c - s_g.c[e];
    double       t  = (el * vl + ec * vc) / (vl * vl + vc * vc);
    t               = t < 0 ? 0 : t > 1 ? 1 : t;
    return std::hypot(el - t * vl, ec - t * vc);
}

// The nearest mix at every lattice target of the area from the strip up, column by column, with the
// ease z. Checks every two entries with weight are neighbours, and that the pick from the last
// triangle moves at most 1 of the 256 ranks. With the linear weights: a target that a triangle holds
// is met within PICK_REACH, and the mix of a target in no triangle lies within 1 of the crossing of
// the focal rule, or with the closest rule of the nearest point of all triangles and pairs, found by
// brute force over the pairs. With the focal rule a target on the top takes within 4 the mix of its
// triangle, and no mix moves over PICK_JUMP between adjacent targets. With an ease: the mix point
// lies within the larger reach t + w of the two steps, plus 1, of the linear mix point. A target 1.5
// beyond the reach of each step that is on takes the linear mix, and with the line step off a target
// 1.5 inside the solid t of the tone of its largest weight takes that tone alone; targets whose two
// largest weights lie within 2% are left out of these two. It reports the jumps over PICK_JUMP and
// the largest move, which inward bends of the rim give, and the largest move between adjacent
// targets where a step acts. The report goes into out.
int pick_run(const tone_near_ease& z, const std::vector<std::array<int, 3>>& tris,
             const std::vector<std::pair<int, int>>& pairs, std::string& out)
{
    const bool           solid_on = tone_near_step_on(z.solid), line_on = tone_near_step_on(z.line);
    const bool           linear = !solid_on && !line_on;
    tone_near_ease       lin    = tone_near_ease_of(0, 0, 0, 0);
    lin.closest                 = z.closest;
    const double         rs = z.solid.t + z.solid.w, rl = z.line.t + z.line.w, reach = std::max(rs, rl);
    int64_t              n = 0, held = 0, rim = 0, steps[TONE_NEAR_STEPS + 1] = {}, jumps = 0, used_n = 0, solid_n = 0;
    int                  pair_bad = 0, reach_bad = 0, out_bad = 0, ease_bad = 0, from_n = 0, from_most = 0, from_bad = 0;
    tone_near_last       last_t;
    double               reach_worst = 0, out_worst = 0, jump_worst = 0, move_worst = 0, zone_jump = 0;
    int                  zone_at[2] = {};
    std::vector<bool>    used(static_cast<size_t>(N), false);
    std::vector<double>  prev_l, prev_c, cur_l, cur_c;
    size_t               last = 0;
    for(int l = 0; l <= TONE_GRAPH_ONE; l++)
    {
        const int top = s_top[static_cast<size_t>(l)];
        cur_l.assign(static_cast<size_t>(std::max(top + 1, 0)), -1), cur_c = cur_l;
        for(int c = TONE_GRAPH_STRIP; c <= top; c++)
        {
            tone_near_mix m, mf, m0;
            tone_near_pick(near_graph{s_g}, z, l, c, &m);
            tone_near_pick(near_graph{s_g}, lin, l, c, &m0);
            n++, steps[m.steps]++, rim += m.how == TONE_NEAR_RIM;
            tone_near_pick_from(near_graph{s_g}, z, &last_t, l, c, &mf);
            if(memcmp(m.v, mf.v, 3) != 0 || memcmp(m.w, mf.w, sizeof(m.w)) != 0)
            {
                int moved = 0;
                for(int r = 0; r < 256; r++) moved += tone_near_choose(m, r) != tone_near_choose(mf, r);
                from_n++, from_most = std::max(from_most, moved);
                if(moved > 1)
                {
                    if(from_bad < 5) say(out, "FAIL pick at %d %d: from the last triangle %d ranks differ\n", l, c, moved);
                    from_bad++;
                }
            }
            for(int i = 0; i < 3; i++)
            {
                if(m.w[i] > 0) used[m.v[i]] = true;
                for(int j = i + 1; j < 3; j++)
                    if(m.w[i] > 0 && m.w[j] > 0 && m.v[i] != m.v[j] && !has(m.v[i], m.v[j]))
                    {
                        if(pair_bad < 5) say(out, "FAIL pick at %d %d: %d and %d mix, not neighbours\n", l, c, m.v[i], m.v[j]);
                        pair_bad++;
                    }
            }
            double ml, mc;
            point_of(m, &ml, &mc);
            bool in = holds(tris[last], l, c);
            for(size_t i = 0; !in && i < tris.size(); i++)
                if(holds(tris[i], l, c)) in = true, last = i;
            const double err = std::hypot(ml - l, mc - c);
            if(in)
            {
                held++;
                reach_worst = std::max(reach_worst, err);
                if(linear && err > PICK_REACH)
                {
                    if(reach_bad < 5) say(out, "FAIL pick at %d %d: a triangle holds it, the mix reaches %.1f %.1f\n", l, c, ml, mc);
                    reach_bad++;
                }
            }
            else if(linear && z.closest)
            {
                double best = 1e30;
                for(const auto& p : pairs) best = std::min(best, seg_dist(p.first, p.second, l, c));
                out_worst = std::max(out_worst, err - best);
                if(err > best + 1)
                {
                    if(out_bad < 5) say(out, "FAIL pick at %d %d: in no triangle, the mix lies %.2f away, the nearest point %.2f\n", l, c, err, best);
                    out_bad++;
                }
            }
            else if(linear)
            {
                // The crossing of the segment from the target to the focal point with the top, found
                // over every edge of the top in double.
                double xl = -1, xc = -1;
                for(int k = 0; k + 1 < s_g.top_n; k++)
                {
                    const double ax = s_g.top_l[k], ay = s_g.top_c[k], bx = s_g.top_l[k + 1], by = s_g.top_c[k + 1];
                    const double dx = TONE_GRAPH_FOCAL_L - l, dy = TONE_GRAPH_FOCAL_C - c, ex = bx - ax, ey = by - ay;
                    const double den = dx * ey - dy * ex;
                    if(den == 0) continue;
                    const double s = ((ax - l) * ey - (ay - c) * ex) / den, u = ((ax - l) * dy - (ay - c) * dx) / den;
                    if(s >= 0 && s <= 1 && u >= 0 && u <= 1) xl = l + s * dx, xc = c + s * dy;
                }
                const double off = xl < 0 ? 1e9 : std::hypot(ml - xl, mc - xc);
                out_worst        = std::max(out_worst, off);
                if(off > 1)
                {
                    if(out_bad < 5) say(out, "FAIL pick at %d %d: in no triangle, the mix lies %.2f from the crossing\n", l, c, off);
                    out_bad++;
                }
            }
            bool zone = false;
            if(!linear)
            {
                // The mix point moves by at most the larger reach of the two steps.
                double l0, c0;
                point_of(m0, &l0, &c0);
                const double move = std::hypot(ml - l0, mc - c0);
                move_worst        = std::max(move_worst, move);
                if(move > reach + 1)
                {
                    if(ease_bad < 5) say(out, "FAIL ease at %d %d: the mix moves %.2f from the linear mix, over %.0f\n", l, c, move, reach);
                    ease_bad++;
                }
                // The distances the steps read: from the target to the nearest edge of a triangle mix,
                // and from the point of the linear mix to the corner of its largest weight.
                const bool tri = m0.how == TONE_NEAR_IN && m0.v[0] != m0.v[1] && m0.v[1] != m0.v[2] && m0.v[0] != m0.v[2];
                double     e   = 1e9;
                if(tri)
                    for(int i = 0; i < 3; i++) e = std::min(e, seg_dist(m0.v[i], m0.v[(i + 1) % 3], l, c));
                int b = 0, b2 = -1;
                for(int i = 1; i < 3; i++)
                    if(m0.w[i] > m0.w[b] || (m0.w[i] == m0.w[b] && m0.v[i] < m0.v[b])) b = i;
                for(int i = 0; i < 3; i++)
                    if(i != b && m0.v[i] != m0.v[b] && (b2 < 0 || m0.w[i] > m0.w[b2])) b2 = i;
                const double s0 = m0.w[0] + m0.w[1] + m0.w[2];
                const bool   tie = b2 >= 0 && m0.w[b] - m0.w[b2] < 0.02 * s0;
                const double d   = std::hypot(l0 - s_g.l[m0.v[b]], c0 - s_g.c[m0.v[b]]);
                const bool   far = (!line_on || !tri || e >= rl + 1.5) && (!solid_on || d >= rs + 1.5);
                zone             = (solid_on && d < rs) || (line_on && tri && e < rl);
                // tone_near_choose() reads the entries and weights alone, so two mixes that hold the
                // same choose the same at every rank.
                const bool same = memcmp(m.v, m0.v, 3) == 0 && memcmp(m.w, m0.w, sizeof(m.w)) == 0;
                if(far && !tie && !same)
                    for(int r = 0; r < 256; r++)
                        if(tone_near_choose(m, r) != tone_near_choose(m0, r))
                        {
                            if(ease_bad < 5) say(out, "FAIL ease at %d %d: beyond the reach of both steps, the mix is not linear\n", l, c);
                            ease_bad++;
                            break;
                        }
                if(solid_on && !line_on && !tie && d <= z.solid.t - 1.5)
                {
                    solid_n++;
                    int with = 0;
                    for(int i = 0; i < 3; i++) with += m.w[i] > 0 && m.v[i] != m0.v[b];
                    if(with)
                    {
                        if(ease_bad < 5) say(out, "FAIL ease at %d %d: %.2f from tone %d, the mix holds another\n", l, c, d, m0.v[b]);
                        ease_bad++;
                    }
                }
            }
            cur_l[static_cast<size_t>(c)] = ml, cur_c[static_cast<size_t>(c)] = mc;
            double jl = c > TONE_GRAPH_STRIP ? std::hypot(ml - cur_l[static_cast<size_t>(c - 1)], mc - cur_c[static_cast<size_t>(c - 1)]) : 0;
            if(static_cast<size_t>(c) < prev_l.size() && prev_l[static_cast<size_t>(c)] >= 0)
                jl = std::max(jl, std::hypot(ml - prev_l[static_cast<size_t>(c)], mc - prev_c[static_cast<size_t>(c)]));
            jump_worst = std::max(jump_worst, jl);
            jumps += jl > PICK_JUMP;
            if(zone && jl > zone_jump) zone_jump = jl, zone_at[0] = l, zone_at[1] = c;
        }
        prev_l.swap(cur_l), prev_c.swap(cur_c);
    }
    for(bool u : used) used_n += u;
    const double q = 1.0 / TONE_GRAPH_ONE;
    char name[80];
    snprintf(name, sizeof(name), "pick %s solid %d %d line %d %d", z.closest ? "closest" : "focal", z.solid.t, z.solid.w,
             z.line.t, z.line.w);
    say(out, "%s: %lld targets, %lld held by a triangle, %lld outside the triangles. Steps across an edge 0 to %d: ", name,
        static_cast<long long>(n), static_cast<long long>(held), static_cast<long long>(rim), TONE_NEAR_STEPS);
    for(int64_t s : steps) say(out, "%lld ", static_cast<long long>(s));
    say(out, "\n%s: held targets missed by up to %.4f%s, %d faults. Pairs not neighbours %d. From the last triangle %d "
        "mixes differ, at most %d of 256 ranks (limit 1), %d faults. %lld of %d tones take dots\n",
        name, reach_worst * q, linear ? " (limit 0.0020)" : "", reach_bad, pair_bad, from_n, from_most, from_bad,
        static_cast<long long>(used_n), N);
    if(linear && z.closest)
        say(out, "%s: targets in no triangle lie up to %.4f farther from the mix than from the nearest point of the "
            "triangles and pairs (limit %.4f), %d faults\n",
            name, out_worst * q, q, out_bad);
    else if(linear)
    {
        // A target on the top takes itself: the focal rule for the target at the top of each l, which
        // lies at most 2 below the exact top, against the mix of step 1 there.
        double top_worst = 0;
        int    top_bad   = 0;
        for(int l = 0; l <= TONE_GRAPH_ONE; l++)
        {
            const int c = tone_graph_top(s_g, l);
            if(c < TONE_GRAPH_STRIP || c > s_top[static_cast<size_t>(l)]) continue;
            tone_near_mix in, cross;
            tone_near_pick(near_graph{s_g}, z, l, c, &in);
            uint8_t t[3];
            int32_t w[3];
            near_graph{s_g}.focal(l, c, t, w);
            tone_near_put(near_graph{s_g}, z, t, w, TONE_NEAR_RIM, l, c, &cross);
            double il, ic, xl, xc;
            point_of(in, &il, &ic), point_of(cross, &xl, &xc);
            const double d = std::hypot(il - xl, ic - xc);
            top_worst      = std::max(top_worst, d);
            if(d > 4)
            {
                if(top_bad < 5) say(out, "FAIL pick at %d %d: on the top, the crossing lies %.2f from the mix in the triangle\n", l, c, d);
                top_bad++;
            }
        }
        out_bad += top_bad;
        say(out, "%s: targets in no triangle lie up to %.4f from the crossing (limit %.4f), %d faults. On the top the "
            "crossing lies up to %.4f from the mix in the triangle (limit %.4f), %d faults\n",
            name, out_worst * q, q, out_bad - top_bad, top_worst * q, 4 * q, top_bad);
    }
    else
        say(out, "%s: the mix moves from the linear mix by up to %.4f (limit %.4f + %.4f), %lld targets checked solid, %d "
            "faults. Where a step acts the mix moves by up to %.4f between adjacent targets (at %d %d)\n",
            name, move_worst * q, reach * q, q, static_cast<long long>(solid_n), ease_bad, zone_jump * q, zone_at[0],
            zone_at[1]);
    // The focal rule moves its point without a step, so with it no mix moves over PICK_JUMP between
    // adjacent targets.
    const int jump_bad = z.closest ? 0 : static_cast<int>(jumps);
    say(out, "%s: %lld targets whose mix moves over %.4f from a target next to it%s, the largest move %.4f\n", name,
        static_cast<long long>(jumps), PICK_JUMP * q, z.closest ? "" : " (limit 0)", jump_worst * q);
    if(jump_bad) say(out, "FAIL pick: %d jumps with the focal rule\n", jump_bad);
    return pair_bad + reach_bad + out_bad + ease_bad + from_bad + jump_bad;
}
} // namespace

int main(int argc, char** argv)
{
    const int brk = argc == 3 && strcmp(argv[1], "--break") == 0 ? atoi(argv[2]) : 0;
    std::copy(tone_tables::graph_cell.begin(), tone_tables::graph_cell.end(), s_cell);
    std::copy(tone_tables::graph_cand.begin(), tone_tables::graph_cand.end(), s_cand);
    if(!tone_graph_hydrate(&s_g, tone_tables::qL.data(), tone_tables::graph_c.data(), tone_tables::neutral.data(),
                           tone_tables::neutral_count, tone_tables::graph_ring.data(), tone_tables::graph_ring_count,
                           tone_tables::graph_top.data(), tone_tables::graph_top_count, s_cell, s_cand,
                           tone_tables::graph_rows))
    {
        printf("FAIL tone_graph_hydrate refuses the rings or the top\n");
        return 1;
    }
    fault(brk);
    make_area();
    make_top();
    int bad = 0;

    // ---- owner: every lattice point of the grid, and two rows below c = 0, in parts of l
    const int H = s_g.rows << TONE_GRAPH_CELL_SHIFT;
    struct own_part
    {
        int         bad = 0, most = 0;
        std::string log;
    };
    const int             parts = host_threads();
    std::vector<own_part> op(static_cast<size_t>(parts));
    std::vector<uint8_t>  img(static_cast<size_t>((TONE_GRAPH_ONE + 1) * H));
    run_parts(TONE_GRAPH_ONE + 1, parts, [&](int lo, int hi, int k) {
        own_part& r = op[static_cast<size_t>(k)];
        for(int l = lo; l < hi; l++)
            for(int c = -2; c < H; c++)
            {
                const int want = brute(l, c, 1);
                const int got  = static_cast<int>(tone_graph_owner(s_g, l, c));
                if(c >= 0) img[static_cast<size_t>(l * H + c)] = static_cast<uint8_t>(want);
                if(c >= 0 && c <= s_top[static_cast<size_t>(l)])
                    r.most = std::max(r.most, static_cast<int>(s_g.cell[std::min(l >> TONE_GRAPH_CELL_SHIFT, TONE_GRAPH_COLS - 1) * s_g.rows +
                                                                        (c >> TONE_GRAPH_CELL_SHIFT)] &
                                                                 15));
                if(got != want)
                {
                    if(r.bad < 5) say(r.log, "FAIL owner of %d %d: %d, nearest %d\n", l, c, got, want);
                    r.bad++;
                }
            }
    });
    int own_bad = 0, most = 0;
    for(const own_part& r : op)
    {
        // The first 5 faults of all parts, in order of l.
        size_t at = 0;
        for(int i = own_bad; i < 5 && at < r.log.size(); i++)
        {
            const size_t nl = r.log.find('\n', at);
            fwrite(r.log.data() + at, 1, nl + 1 - at, stdout);
            at = nl + 1;
        }
        own_bad += r.bad, most = std::max(most, r.most);
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

    // ---- toward: from the owner to targets of its region, and from each tone in 64 directions
    tow_count tw;
    for(int l = 3; l <= TONE_GRAPH_ONE; l += 13)
        for(int c = TONE_GRAPH_STRIP; c <= s_top[static_cast<size_t>(l)]; c += 5)
        {
            const int a = img[static_cast<size_t>(l * H + c)];
            toward_one(a, l - s_g.l[a], c - s_g.c[a], tw);
        }
    for(int a = 0; a < N; a++)
        for(int k = 0; k < 64; k++)
            toward_one(a, static_cast<int>(std::lround(1000 * std::cos(k * M_PI / 32))),
                       static_cast<int>(std::lround(1000 * std::sin(k * M_PI / 32))), tw);
    bad += tw.bad;
    printf("toward: %d targets, %d with no neighbour under 90 degrees, %d near ties, %d faults\n", tw.n, tw.none, tw.tie,
           tw.bad);

    // ---- top
    int top_bad = 0, top_none = 0, top_low = 0;
    for(int l = 0; l <= TONE_GRAPH_ONE; l++)
    {
        const top_at t   = exact_top(gt, l);
        const int    got = tone_graph_top(s_g, l);
        int          hi = 0, lo = 0;
        if(t.num >= 0)
        {
            const int f = static_cast<int>(t.num / t.den);
            hi = f, lo = std::max(f - 2, 0);
        }
        top_none += t.num < 0;
        top_low += got < hi;
        if(got < lo || got > hi)
        {
            if(top_bad < 5) printf("FAIL top at l %d: %d, the triangles reach %d\n", l, got, hi);
            top_bad++;
        }
    }
    bad += top_bad;
    printf("top: %d corners, %d of %d l with no triangle, %d below the exact top by 1 or 2, %d faults\n", s_g.top_n,
           top_none, TONE_GRAPH_ONE + 1, top_low, top_bad);

    // ---- pick, with the linear weights, a step at 8 and the default ease, each on a thread
    {
        const std::vector<std::array<int, 3>> tris(gt.begin(), gt.end());
        std::vector<std::pair<int, int>>      pairs;
        for(int e = 0; e < N; e++)
            for(unsigned k = s_g.at[e]; k < s_g.at[e + 1]; k++)
                if(s_g.nb[k] > e) pairs.push_back({e, s_g.nb[k]});
        auto closest = [](tone_near_ease z) {
            z.closest = true;
            return z;
        };
        const std::vector<tone_near_ease> zs = {tone_near_ease_of(0, 0, 0, 0),         tone_near_ease_of(8, 0, 0, 0),
                                                tone_near_ease_of(8, 16, 0, 0),        tone_near_ease_of(0, 0, 8, 16),
                                                tone_near_ease_of(8, 16, 8, 16),       closest(tone_near_ease_of(0, 0, 0, 0)),
                                                closest(tone_near_ease_of(8, 16, 0, 0))};
        std::vector<std::string>          logs(zs.size());
        std::vector<int>                  fails(zs.size());
        std::vector<std::thread>          th;
        for(size_t i = 0; i < zs.size(); i++) th.emplace_back([&, i] { fails[i] = pick_run(zs[i], tris, pairs, logs[i]); });
        for(size_t i = 0; i < zs.size(); i++)
        {
            th[i].join();
            fputs(logs[i].c_str(), stdout);
            bad += fails[i];
        }
    }

    printf("tone_graph_check%s: %s\n", brk ? " with a fault" : "", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
