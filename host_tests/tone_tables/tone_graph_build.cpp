// The graph of the tones in the plane of L and 2C, q12 (main/tone_graph.h). Each region is cut from
// its domain by the half planes of its bisectors. A polygon is the cycle of the lines of its edges
// and each vertex is the meeting point of two integer lines, so every test is exact and no error
// builds up. A region's border with the domain, with another tone or with the strip is read from the
// line each edge lies on.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "tone_fx_squeeze_internal.h"
#include "tone_graph.h"
#include "tone_graph_build.h"
#include "tonemap.h"

namespace
{
typedef __int128 i128;

// The tag of a line that is not the bisector of a tone.
const int     AREA = -1, STRIP = -2;
const int64_t BIG  = 1 << 14;

// a x + b y <= c. Its tag names what lies beyond it.
struct line
{
    int64_t a, b, c;
    int     tag;
};

// The point (x / d, y / d), d above 0.
struct pt
{
    i128 x, y, d;
};

// x = n / d on the line c = TONE_GRAPH_STRIP, d above 0.
struct rat
{
    i128 n, d;
};

bool s_bad = false;

pt meet(const line& p, const line& q)
{
    i128 d = static_cast<i128>(p.a) * q.b - static_cast<i128>(q.a) * p.b;
    i128 x = static_cast<i128>(p.c) * q.b - static_cast<i128>(q.c) * p.b;
    i128 y = static_cast<i128>(p.a) * q.c - static_cast<i128>(q.a) * p.c;
    if(d == 0) s_bad = true, d = 1;
    if(d < 0) d = -d, x = -x, y = -y;
    return {x, y, d};
}

int side(const line& h, const pt& v)
{
    const i128 s = static_cast<i128>(h.a) * v.x + static_cast<i128>(h.b) * v.y - static_cast<i128>(h.c) * v.d;
    return s < 0 ? -1 : s > 0;
}

bool same(const pt& p, const pt& q)
{
    return p.x * q.d == q.x * p.d && p.y * q.d == q.y * p.d;
}

bool lt(const rat& p, const rat& q)
{
    return p.n * q.d < q.n * p.d;
}

i128 gcd(i128 a, i128 b)
{
    if(a < 0) a = -a;
    if(b < 0) b = -b;
    while(b != 0)
    {
        const i128 t = a % b;
        a = b, b = t;
    }
    return a;
}

std::tuple<i128, i128, i128> key(const pt& p)
{
    i128 g = gcd(gcd(p.x, p.y), p.d);
    if(g == 0) g = 1;
    return {p.x / g, p.y / g, p.d / g};
}

// Edges counter clockwise. Vertex k is where edge k - 1 meets edge k.
typedef std::vector<line> poly;

pt vertex(const poly& p, size_t k)
{
    const size_t m = p.size();
    return meet(p[(k + m - 1) % m], p[k % m]);
}

// p cut to h. Empty when no part of non zero area remains.
poly clip(const poly& p, const line& h)
{
    const size_t m = p.size();
    if(m == 0) return p;
    std::vector<int> s(m);
    bool             in = false, out = false;
    for(size_t k = 0; k < m; k++)
    {
        s[k] = side(h, vertex(p, k));
        in   = in || s[k] < 0;
        out  = out || s[k] > 0;
    }
    if(!out) return p;
    if(!in) return {};
    poly r;
    for(size_t k = 0; k < m; k++)
    {
        const int s0 = s[k], s1 = s[(k + 1) % m];
        if(std::min(s0, s1) < 0) r.push_back(p[k]);
        if(s0 <= 0 && s1 > 0) r.push_back(h);
    }
    return r.size() < 3 ? poly() : r;
}

// p without its edges of zero length, which three lines through one point leave.
poly tidy(poly p)
{
    for(bool again = true; again && p.size() >= 3;)
    {
        again = false;
        for(size_t k = 0; k < p.size() && !again; k++)
            if(same(vertex(p, k), vertex(p, k + 1))) p.erase(p.begin() + static_cast<long>(k)), again = true;
    }
    return p.size() < 3 ? poly() : p;
}

poly box()
{
    return {{0, -1, BIG, AREA}, {1, 0, BIG, AREA}, {0, 1, BIG, AREA}, {-1, 0, BIG, AREA}};
}

struct tone_pt
{
    int64_t x, y;
};

struct graph_in
{
    int                  n;
    std::vector<tone_pt> p;
    std::vector<bool>    neutral, shadow;
};

// The points nearer to e than to f, or as near.
line bisector(const graph_in& g, int e, int f)
{
    const tone_pt &u = g.p[static_cast<size_t>(e)], &v = g.p[static_cast<size_t>(f)];
    return {2 * (v.x - u.x), 2 * (v.y - u.y), v.x * v.x + v.y * v.y - u.x * u.x - u.y * u.y, f};
}

// The part of e's region at or above the strip line (up), where every tone competes, or at or below
// it, where the neutral tones do.
poly piece(const graph_in& g, int e, const std::vector<line>& dom, bool up)
{
    if(g.shadow[static_cast<size_t>(e)] || (!up && !g.neutral[static_cast<size_t>(e)])) return {};
    poly p = box();
    for(const line& h : dom) p = clip(p, h);
    p = clip(p, up ? line{0, -1, -TONE_GRAPH_STRIP, STRIP} : line{0, 1, TONE_GRAPH_STRIP, STRIP});
    for(int f = 0; f < g.n && !p.empty(); f++)
    {
        if(f == e || g.shadow[static_cast<size_t>(f)] || (!up && !g.neutral[static_cast<size_t>(f)])) continue;
        p = clip(p, bisector(g, e, f));
    }
    return tidy(p);
}

struct seg
{
    pt  a, b;
    int tag;
};

// Along the strip line: the bands of the neutral tones below it, and the stretch of each tone's
// upper part on it.
struct strip_map
{
    std::vector<rat> cut;
    std::vector<int> band;
    std::vector<int> owner;
    std::vector<rat> x0, x1;
};

strip_map strip_of(const graph_in& g, const std::vector<poly>& up)
{
    strip_map        s;
    std::vector<int> nt;
    for(int e = 0; e < g.n; e++)
        if(g.neutral[static_cast<size_t>(e)] && !g.shadow[static_cast<size_t>(e)]) nt.push_back(e);
    std::sort(nt.begin(), nt.end(), [&](int a, int b) { return g.p[static_cast<size_t>(a)].x < g.p[static_cast<size_t>(b)].x; });
    for(size_t i = 0; i < nt.size(); i++)
    {
        s.band.push_back(nt[i]);
        if(i > 0) s.cut.push_back({g.p[static_cast<size_t>(nt[i - 1])].x + g.p[static_cast<size_t>(nt[i])].x, 2});
    }
    for(int e = 0; e < g.n; e++)
    {
        const poly& p = up[static_cast<size_t>(e)];
        for(size_t k = 0; k < p.size(); k++)
        {
            if(p[k].tag != STRIP) continue;
            const pt a = vertex(p, k), b = vertex(p, k + 1);
            if(same(a, b)) continue;
            rat ra = {a.x, a.d}, rb = {b.x, b.d};
            if(lt(rb, ra)) std::swap(ra, rb);
            s.owner.push_back(e), s.x0.push_back(ra), s.x1.push_back(rb);
        }
    }
    return s;
}

// Who owns the stretch lo to hi of the strip line, below it (the band) or above it.
int band_at(const strip_map& s, const rat& lo, const rat& hi)
{
    for(size_t i = 0; i < s.band.size(); i++)
        if((i == 0 || !lt(lo, s.cut[i - 1])) && (i == s.band.size() - 1 || !lt(s.cut[i], hi))) return s.band[i];
    s_bad = true;
    return AREA;
}

int above_at(const strip_map& s, const rat& lo, const rat& hi)
{
    for(size_t i = 0; i < s.owner.size(); i++)
        if(!lt(lo, s.x0[i]) && !lt(s.x1[i], hi)) return s.owner[i];
    s_bad = true;
    return AREA;
}

// The border pieces of p, each edge with the tone or the domain beyond it, as directed segments.
// An edge on the strip line splits where the owner across it changes. Pieces with self beyond them
// lie inside self's region and are left out.
void segments(const poly& p, bool up, int self, const strip_map& s, std::vector<seg>& out)
{
    for(size_t k = 0; k < p.size(); k++)
    {
        const pt a = vertex(p, k), b = vertex(p, k + 1);
        if(same(a, b)) continue;
        if(p[k].tag != STRIP)
        {
            if(p[k].tag != self) out.push_back({a, b, p[k].tag});
            continue;
        }
        const rat        ra = {a.x, a.d}, rb = {b.x, b.d};
        const bool       fwd = lt(ra, rb);
        std::vector<rat> at = {ra};
        std::vector<rat> mid;
        for(const rat& c : s.cut) mid.push_back(c);
        for(size_t i = 0; i < s.x0.size(); i++) mid.push_back(s.x0[i]), mid.push_back(s.x1[i]);
        std::sort(mid.begin(), mid.end(), [&](const rat& u, const rat& v) { return fwd ? lt(u, v) : lt(v, u); });
        for(const rat& c : mid)
        {
            const bool inside = fwd ? lt(ra, c) && lt(c, rb) : lt(rb, c) && lt(c, ra);
            if(inside && lt(at.back(), c) != lt(c, at.back())) at.push_back(c);
        }
        at.push_back(rb);
        for(size_t i = 0; i + 1 < at.size(); i++)
        {
            const rat lo  = fwd ? at[i] : at[i + 1], hi = fwd ? at[i + 1] : at[i];
            const int tag = up ? band_at(s, lo, hi) : above_at(s, lo, hi);
            const pt  u   = {at[i].n, static_cast<i128>(TONE_GRAPH_STRIP) * at[i].d, at[i].d};
            const pt  v   = {at[i + 1].n, static_cast<i128>(TONE_GRAPH_STRIP) * at[i + 1].d, at[i + 1].d};
            if(tag != self) out.push_back({u, v, tag});
        }
    }
}

// The tags around e's region counter clockwise, equal neighbours merged. Several cycles when the
// region has several parts. False when the borders do not close.
bool rings_of(const std::vector<seg>& sg, std::vector<std::vector<int>>& cyc)
{
    std::map<std::tuple<i128, i128, i128>, std::vector<size_t>> from;
    for(size_t i = 0; i < sg.size(); i++) from[key(sg[i].a)].push_back(i);
    for(const auto& kv : from)
        if(kv.second.size() != 1) return false;
    std::vector<bool> used(sg.size(), false);
    cyc.clear();
    for(size_t s0 = 0; s0 < sg.size(); s0++)
    {
        if(used[s0]) continue;
        std::vector<int> c;
        size_t           i = s0;
        while(!used[i])
        {
            used[i] = true;
            if(c.empty() || c.back() != sg[i].tag) c.push_back(sg[i].tag);
            const auto it = from.find(key(sg[i].b));
            if(it == from.end()) return false;
            i = it->second[0];
        }
        if(i != s0) return false;
        if(c.size() > 1 && c.front() == c.back()) c.pop_back();
        // A ring with a stretch of the domain's edge ends with it.
        const auto ar = std::find(c.begin(), c.end(), AREA);
        if(ar != c.end()) std::rotate(c.begin(), ar + 1, c.end());
        cyc.push_back(c);
    }
    return true;
}

void put_list(std::string& s, const std::vector<int>& v, int ind)
{
    char buf[32];
    for(size_t i = 0; i < v.size(); i++)
    {
        if(i % 16 == 0) s.append(static_cast<size_t>(ind), ' ');
        snprintf(buf, sizeof(buf), "%d,", v[i]);
        s += buf;
        s += i % 16 == 15 || i == v.size() - 1 ? "\n" : " ";
    }
}

// The domain of the targets in the plane. pan_edge() of the squeeze is at most the least of its
// four lines, C <= PM L / 2^15 + PB with PB 0 for the first, as a real number. A point (l, c) comes
// from some L in 8 l - 4 to 8 l + 3 and some C of at least 4 c - 2, so each line gives the half plane
// 2^17 c - 8 PM l <= PM d + 2^15 PB + 2^16 with d 3 for PM of 0 or above and -4 below.
std::vector<line> area_lines(const int16_t* k)
{
    using namespace tone_fx_squeeze_internal;
    std::vector<line> h = {{-1, 0, -tone_graph_l12(k[K_LB]), AREA}, {1, 0, TONE_GRAPH_ONE, AREA}, {0, -1, 0, AREA}};
    const int64_t     pm[4] = {k[K_PM0], k[K_PM1], k[K_PM2], k[K_PM3]};
    const int64_t     pb[4] = {0, k[K_PB1], k[K_PB2], k[K_PB3]};
    for(int i = 0; i < 4; i++) h.push_back({-8 * pm[i], 131072, pm[i] * (pm[i] >= 0 ? 3 : -4) + 32768 * pb[i] + 65536, AREA});
    return h;
}
} // namespace

bool tone_graph_build(const int16_t* qL, const int16_t* k, std::string& inc, char* why, size_t len)
{
    using namespace tone_fx_squeeze_internal;
    const int N = static_cast<int>(TONEMAP_COUNT);
    graph_in  g;
    g.n = N;
    std::vector<int> gc(static_cast<size_t>(N));
    for(int e = 0; e < N; e++)
    {
        const tone_entry& t  = k_tonemap[e];
        const double      ch = std::sqrt(static_cast<double>(t.a) * t.a + static_cast<double>(t.b) * t.b);
        const long        cq = std::min(32767L, std::lround(ch * 32768.0));
        gc[static_cast<size_t>(e)] = tone_graph_c12(static_cast<int>(cq));
        g.p.push_back({tone_graph_l12(qL[e]), gc[static_cast<size_t>(e)]});
        g.neutral.push_back(!strpbrk(t.pattern, "rR"));
        bool shadow = false;
        for(int f = 0; f < e; f++)
            shadow = shadow || (g.p[static_cast<size_t>(f)].x == g.p.back().x && g.p[static_cast<size_t>(f)].y == g.p.back().y);
        g.shadow.push_back(shadow);
        if(!g.neutral.back() && g.p.back().y < TONE_GRAPH_STRIP)
        {
            snprintf(why, len, "coloured entry %d (%s) lies in the strip, at 2C %lld of q12", e, t.pattern,
                     static_cast<long long>(g.p.back().y));
            return false;
        }
    }
    // Below the floor K_LB a squeezed L rises to it with the chroma of its own L, which the area
    // covers while the panel edge does not fall below the floor.
    for(int l = 0; l < k[K_LB]; l++)
        if(pan_edge(k, static_cast<int16_t>(l + 1)) < pan_edge(k, static_cast<int16_t>(l)))
        {
            snprintf(why, len, "the panel edge falls below the floor of the squeeze, at L %d of q15", l);
            return false;
        }

    const std::vector<line> area = area_lines(k);
    poly                    whole = box();
    for(const line& h : area) whole = clip(whole, h);
    whole = tidy(whole);
    if(whole.empty())
    {
        snprintf(why, len, "the area of the targets is empty");
        return false;
    }

    // ---- the regions in the area and their rings
    std::vector<poly> up(static_cast<size_t>(N)), low(static_cast<size_t>(N));
    for(int e = 0; e < N; e++)
        up[static_cast<size_t>(e)] = piece(g, e, area, true), low[static_cast<size_t>(e)] = piece(g, e, area, false);
    const strip_map               sm = strip_of(g, up);
    std::vector<std::vector<int>> ring(static_cast<size_t>(N));
    int                           none = 0;
    for(int e = 0; e < N; e++)
    {
        std::vector<seg> sg;
        segments(up[static_cast<size_t>(e)], true, e, sm, sg);
        segments(low[static_cast<size_t>(e)], false, e, sm, sg);
        std::vector<std::vector<int>> cyc;
        if(!rings_of(sg, cyc))
        {
            snprintf(why, len, "the borders of entry %d do not close", e);
            return false;
        }
        if(cyc.size() > 1)
        {
            snprintf(why, len, "the region of entry %d has %zu parts", e, cyc.size());
            return false;
        }
        none += cyc.empty();
        if(!cyc.empty()) ring[static_cast<size_t>(e)] = cyc[0];
    }
    if(s_bad)
    {
        snprintf(why, len, "the geometry met parallel lines or a stretch of the strip without owner");
        return false;
    }

    // Neighbours must agree, and each triangle must show in the rings of its three tones.
    std::set<std::pair<int, int>> pairs;
    for(int e = 0; e < N; e++)
        for(int f : ring[static_cast<size_t>(e)])
            if(f >= 0) pairs.insert({e, f});
    for(const auto& pr : pairs)
        if(!pairs.count({pr.second, pr.first}))
        {
            snprintf(why, len, "entry %d borders %d but not the other way", pr.first, pr.second);
            return false;
        }
    std::map<std::array<int, 3>, std::set<int>> tri;
    int                                         deg_max = 0;
    for(int e = 0; e < N; e++)
    {
        const std::vector<int>& r = ring[static_cast<size_t>(e)];
        deg_max = std::max(deg_max, static_cast<int>(std::count_if(r.begin(), r.end(), [](int f) { return f >= 0; })));
        for(size_t i = 0; i < r.size(); i++)
        {
            const int f = r[i], h = r[(i + 1) % r.size()];
            if(f < 0 || h < 0 || f == h || !pairs.count({f, h})) continue;
            std::array<int, 3> t = {e, f, h};
            std::sort(t.begin(), t.end());
            tri[t].insert(e);
        }
    }
    for(const auto& kv : tri)
        if(kv.second.size() != 3)
        {
            snprintf(why, len, "triangle %d %d %d shows in %zu rings, not 3", kv.first[0], kv.first[1], kv.first[2],
                     kv.second.size());
            return false;
        }

    // ---- the owner grid over l 0 to 4096 and c 0 to the top of its last row, which holds the area
    i128 top = 0;
    for(size_t v = 0; v < whole.size(); v++)
    {
        const pt q = vertex(whole, v);
        top        = std::max(top, q.y / q.d);
    }
    const int         S = TONE_GRAPH_CELL_SHIFT, cols = TONE_GRAPH_COLS;
    const int         rows = static_cast<int>(top >> S) + 1;
    std::vector<line> boxd = {{-1, 0, 0, AREA}, {1, 0, TONE_GRAPH_ONE, AREA}, {0, -1, 0, AREA},
                              {0, 1, (static_cast<int64_t>(rows) << S) - 1, AREA}};
    std::vector<poly> bup(static_cast<size_t>(N)), blow(static_cast<size_t>(N));
    for(int e = 0; e < N; e++)
        bup[static_cast<size_t>(e)] = piece(g, e, boxd, true), blow[static_cast<size_t>(e)] = piece(g, e, boxd, false);
    std::vector<int>                       cell(static_cast<size_t>(cols * rows));
    std::vector<int>                       cand;
    std::map<std::vector<int>, int>        shared;
    std::vector<int>                       count(static_cast<size_t>(cols * rows));
    for(int i = 0; i < cols; i++)
        for(int j = 0; j < rows; j++)
        {
            const int64_t x0 = static_cast<int64_t>(i) << S, x1 = i == cols - 1 ? TONE_GRAPH_ONE : ((i + 1LL) << S) - 1;
            const int64_t y0 = static_cast<int64_t>(j) << S, y1 = ((j + 1LL) << S) - 1;
            // The cell's lattice points and half a step around them, so a region that touches a
            // point at a corner or along a side still counts.
            const line r[4] = {{-2, 0, -2 * x0 + 1, AREA}, {2, 0, 2 * x1 + 1, AREA}, {0, -2, -2 * y0 + 1, AREA},
                               {0, 2, 2 * y1 + 1, AREA}};
            std::vector<int> list;
            for(int e = 0; e < N; e++)
            {
                bool hit = false;
                for(const poly* p : {&bup[static_cast<size_t>(e)], &blow[static_cast<size_t>(e)]})
                {
                    poly q = *p;
                    for(const line& h : r) q = clip(q, h);
                    hit = hit || !q.empty();
                }
                if(hit) list.push_back(e);
            }
            const int at = i * rows + j;
            if(list.empty() || list.size() > 15)
            {
                snprintf(why, len, "cell %d %d of the owner grid has %zu candidates", i, j, list.size());
                return false;
            }
            auto it = shared.find(list);
            if(it == shared.end())
            {
                it = shared.insert({list, static_cast<int>(cand.size())}).first;
                cand.insert(cand.end(), list.begin(), list.end());
            }
            if(it->second > 4095)
            {
                snprintf(why, len, "the candidate lists run past 4096 bytes");
                return false;
            }
            cell[static_cast<size_t>(at)]  = it->second << 4 | static_cast<int>(list.size());
            count[static_cast<size_t>(at)] = static_cast<int>(list.size());
        }
    if(s_bad)
    {
        snprintf(why, len, "the geometry of the owner grid met parallel lines");
        return false;
    }
    // The mean list over the lattice points of the area.
    double   sum = 0;
    int64_t  pts = 0;
    int      cmax = 0;
    for(int x = 0; x <= TONE_GRAPH_ONE; x++)
    {
        int64_t ym = INT64_MAX;
        for(const line& h : area)
            if(h.b > 0)
            {
                const int64_t q = h.c - h.a * x;
                ym              = std::min(ym, q >= 0 ? q / h.b : -((-q + h.b - 1) / h.b));
            }
        if(ym < 0) continue;
        const int i = std::min(x >> S, cols - 1);
        for(int64_t y = 0; y <= ym; y++)
        {
            const int c = count[static_cast<size_t>(i * rows + static_cast<int>(y >> S))];
            sum += c, pts++, cmax = std::max(cmax, c);
        }
    }

    // ---- the text
    std::vector<int> rb;
    for(int e = 0; e < N; e++)
    {
        for(int f : ring[static_cast<size_t>(e)]) rb.push_back(f >= 0 ? f : TONE_GRAPH_GAP);
        rb.push_back(TONE_GRAPH_END);
    }
    char buf[512];
    inc += "// The graph of the tones in the plane of L and 2C, q12 (tone_graph.h): per entry 2C, the rings of\n"
           "// neighbours, and the owner grid with its lists of candidates.\n";
    inc += "inline constexpr std::array<int16_t, count> graph_c = {{\n";
    put_list(inc, gc, 4);
    snprintf(buf, sizeof(buf), "}};\ninline constexpr int graph_ring_count = %zu;\n", rb.size());
    inc += buf;
    inc += "inline constexpr std::array<uint8_t, graph_ring_count> graph_ring = {{\n";
    put_list(inc, rb, 4);
    snprintf(buf, sizeof(buf), "}};\ninline constexpr int graph_cols = %d;\ninline constexpr int graph_rows = %d;\n", cols,
             rows);
    inc += buf;
    inc += "inline constexpr std::array<uint16_t, graph_cols * graph_rows> graph_cell = {{\n";
    put_list(inc, cell, 4);
    snprintf(buf, sizeof(buf), "}};\ninline constexpr int graph_cand_count = %zu;\n", cand.size());
    inc += buf;
    inc += "inline constexpr std::array<uint8_t, graph_cand_count> graph_cand = {{\n";
    put_list(inc, cand, 4);
    inc += "}};\n";

    printf("tone_graph_build: %d entries, %d without a region, %zu neighbour pairs, %zu triangles, at most %d "
           "neighbours, ring %zu bytes, owner grid %d x %d cells, at most %d candidates at a point of the area, mean "
           "%.2f over its %lld points, %zu candidate bytes\n",
           N, none, pairs.size() / 2, tri.size(), deg_max, rb.size(), cols, rows, cmax, sum / static_cast<double>(pts),
           static_cast<long long>(pts), cand.size());
    return true;
}
