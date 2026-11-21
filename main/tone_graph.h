// The graph of the tones in the plane of L and 2C, both q12. C is the chroma of a tone, the length of
// its a and b, and the chroma of a target, which the squeeze has put on the red hue. The 2 is the
// chroma weight of the search (TONEMAP_CHROMA_WEIGHT), so the distance of the plane is the distance
// of the search for a point on the hue.
//
// Every point of the area of targets has an owner, the nearest tone. Below c = TONE_GRAPH_STRIP only
// the neutral tones compete, so the gray axis belongs to the grays, paper and ink, and a coloured
// tone's region ends at that line. Ties go to the lower entry. Two tones are neighbours when their
// regions share a border of non zero length inside the area. Three tones form a triangle when they
// are neighbours of each other and their regions meet at a point of the area. The area is the set
// of points a squeezed target can take: l from 0 to 4096, c from 0 to the panel edge of the squeeze,
// with the rounding of tone_graph_l12() and tone_graph_c12() included.
//
// The top of the triangles is, per l, the highest c that a triangle of the graph reaches. It is a
// polyline of a few corners. Above it no mix of three neighbouring tones reaches a target.
//
// host_tests/tone_tables builds the graph with exact integer geometry and writes it to
// main/tone_tables.inc in a compact form:
//   graph_c     per entry its c. Its l comes from qL.
//   graph_ring  per entry its neighbours in the counter clockwise order of the borders around its
//               region, TONE_GRAPH_GAP where the border runs along the edge of the area, then
//               TONE_GRAPH_END. A gap at the end of a ring lies between its last and first border.
//   graph_cell  per cell of the owner grid (column l >> TONE_GRAPH_CELL_SHIFT, row c >> the same),
//               the start of its list in graph_cand in the high 12 bits and its length in the low 4.
//   graph_cand  per cell the entries, lowest first, whose regions reach the cell or lie within half
//               a step of it. Equal lists share their bytes.
//   graph_top   the corners of the top of the triangles as pairs l, c, by rising l from 0 to 4096.
//               Two corners at one l mark a step of the top. Each c is rounded down. Where no
//               triangle reaches, c is 0.
// tone_graph_hydrate() turns the rings into the arrays of tone_graph once at init.
#pragma once

#include <cstdint>

#include "tonemap.h"

static const int     TONE_GRAPH_ONE        = 4096;
static const int     TONE_GRAPH_STRIP      = 8;
static const int     TONE_GRAPH_CELL_SHIFT = 7;
static const int     TONE_GRAPH_COLS       = TONE_GRAPH_ONE >> TONE_GRAPH_CELL_SHIFT;
static const uint8_t TONE_GRAPH_GAP        = 0xfe;
static const uint8_t TONE_GRAPH_END        = 0xff;
static const uint8_t TONE_GRAPH_TRI        = 1;
// A planar graph of n vertices has at most 3 n - 6 edges, so its rings hold below 6 n slots.
static const int TONE_GRAPH_SLOTS = 6 * static_cast<int>(TONEMAP_COUNT);
static const int TONE_GRAPH_TOP_MAX = 48;
static const int TONE_GRAPH_RIM_MAX = 64;
// The focal point of the focal rule (tone_graph_focal()), on the gray axis at L 0.513. Of the points
// that keep the top star shaped, it gives the targets above the top the least mean distance to their
// mix: 0.0298 of the plane, against 0.0251 for the closest rule, the nearest point of the rim
// (probes/tone_graph). A build may set another l, as the test of the generator's refusal does.
#ifndef TONE_GRAPH_FOCAL_AT_L
#define TONE_GRAPH_FOCAL_AT_L 2100
#endif
static const int TONE_GRAPH_FOCAL_L = TONE_GRAPH_FOCAL_AT_L;
static const int TONE_GRAPH_FOCAL_C = 0;
// The length of a unit vector of tone_graph.
static const int TONE_GRAPH_UNIT = 1 << 14;

static_assert(TONEMAP_COUNT < TONE_GRAPH_GAP, "a ring holds an entry in a byte below the markers");
static_assert(1 << TONE_GRAPH_CELL_SHIFT >= TONE_GRAPH_STRIP, "the strip lies in the first row of cells");

// A target's L and C, q15, in the plane.
constexpr int tone_graph_l12(int l)
{
    return (l + 4) >> 3;
}
constexpr int tone_graph_c12(int c)
{
    return (c + 2) >> 2;
}

// fl[k] has TONE_GRAPH_TRI when the tone, nb[k] and the next neighbour of the ring form a triangle.
// (ul[k], uc[k]) is the vector from the tone to nb[k] at the length TONE_GRAPH_UNIT, rounded. The top
// of the triangles has top_n corners (top_l, top_c), and top_s[k] is the slope from corner k to
// k + 1 in q16, rounded down, or 0 at a step. top_e[k] is the tone at corner k: every corner is a
// tone, each two corners in a row are neighbours, and the corners turn one way seen from the focal
// point, so the top is star shaped from it. The rim is the border of the union of the triangles
// and of the pairs in no triangle: the rim_n pairs with a triangle on one side or none, rim_a to
// rim_b, rim_t the corner of the triangle on the other side or TONE_GRAPH_END, (rim_ul, rim_uc) the
// unit vector from rim_a to rim_b, and rim_lo and rim_hi the least and the most l of the pair, by
// rising rim_lo.
struct tone_graph
{
    int16_t         l[TONEMAP_COUNT];
    int16_t         c[TONEMAP_COUNT];
    uint8_t         neutral[TONEMAP_COUNT];
    uint16_t        at[TONEMAP_COUNT + 1];
    uint8_t         nb[TONE_GRAPH_SLOTS];
    uint8_t         fl[TONE_GRAPH_SLOTS];
    int16_t         ul[TONE_GRAPH_SLOTS];
    int16_t         uc[TONE_GRAPH_SLOTS];
    int16_t         top_l[TONE_GRAPH_TOP_MAX];
    int16_t         top_c[TONE_GRAPH_TOP_MAX];
    int32_t         top_s[TONE_GRAPH_TOP_MAX];
    int             top_n;
    uint8_t         top_e[TONE_GRAPH_TOP_MAX];
    uint8_t         rim_a[TONE_GRAPH_RIM_MAX];
    uint8_t         rim_b[TONE_GRAPH_RIM_MAX];
    uint8_t         rim_t[TONE_GRAPH_RIM_MAX];
    int16_t         rim_ul[TONE_GRAPH_RIM_MAX];
    int16_t         rim_uc[TONE_GRAPH_RIM_MAX];
    int16_t         rim_lo[TONE_GRAPH_RIM_MAX];
    int16_t         rim_hi[TONE_GRAPH_RIM_MAX];
    int             rim_n;
    const uint16_t* cell;
    const uint8_t*  cand;
    int             rows;
};

// The integer square root of v, rounded down.
inline uint64_t tone_graph_isqrt(uint64_t v)
{
    uint64_t r = 0;
    for(uint64_t b = uint64_t(1) << 62; b != 0; b >>= 2)
        if(v >= r + b)
        {
            v -= r + b;
            r = (r >> 1) + b;
        }
        else
            r >>= 1;
    return r;
}

// n / d rounded to the nearest, halves away from 0, for d above 0.
inline int64_t tone_graph_div_round(int64_t n, int64_t d)
{
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

inline int tone_graph_third(const tone_graph& g, unsigned a, unsigned b, int side);

// Fills g from the tables of main/tone_tables.inc: qL and graph_c per entry, the neutral entries,
// the rings, the n_top corners of the top of the triangles as pairs l, c and the owner grid of rows
// rows. cell and cand stay where they are and must stay valid. False when a ring runs past the end of
// ring or past TONE_GRAPH_SLOTS, names no entry or names the entry itself, or when the corners do not
// fit, fall in l or do not run from l 0 to TONE_GRAPH_ONE, when a corner is no tone, two corners in a
// row are no neighbours or the top is not star shaped from the focal point, or when the rim has more
// than TONE_GRAPH_RIM_MAX pairs.
inline bool tone_graph_hydrate(tone_graph* g, const int16_t* qL, const int16_t* gc, const uint8_t* neutral, int n_neutral,
                               const uint8_t* ring, int n_ring, const int16_t* top, int n_top, const uint16_t* cell,
                               const uint8_t* cand, int rows)
{
    const unsigned N = static_cast<unsigned>(TONEMAP_COUNT);
    uint8_t        gap[TONE_GRAPH_SLOTS];
    unsigned       k = 0;
    int            r = 0;
    for(unsigned e = 0; e < N; e++)
    {
        g->l[e]       = static_cast<int16_t>(tone_graph_l12(qL[e]));
        g->c[e]       = gc[e];
        g->neutral[e] = 0;
        g->at[e]      = static_cast<uint16_t>(k);
        for(;; r++)
        {
            if(r >= n_ring) return false;
            const uint8_t v = ring[r];
            if(v == TONE_GRAPH_END) break;
            if(v == TONE_GRAPH_GAP)
            {
                if(k == g->at[e]) return false;
                gap[k - 1] = 1;
                continue;
            }
            if(v >= N || v == e || k >= static_cast<unsigned>(TONE_GRAPH_SLOTS)) return false;
            g->nb[k]  = v;
            gap[k++]  = 0;
        }
        r++;
    }
    g->at[N] = static_cast<uint16_t>(k);
    for(int i = 0; i < n_neutral; i++)
        if(neutral[i] < N) g->neutral[neutral[i]] = 1;
    // A triangle needs the two borders to meet with no stretch of the area's edge between them, and
    // the two neighbours to be neighbours of each other.
    for(unsigned e = 0; e < N; e++)
    {
        const unsigned k0 = g->at[e], deg = g->at[e + 1] - k0;
        for(unsigned j = 0; j < deg; j++)
        {
            const unsigned f = g->nb[k0 + j], h = g->nb[k0 + (j + 1) % deg];
            bool           mutual = false;
            for(unsigned i = g->at[f]; i < g->at[f + 1]; i++) mutual = mutual || g->nb[i] == h;
            g->fl[k0 + j] = !gap[k0 + j] && f != h && mutual ? TONE_GRAPH_TRI : 0;
            // The unit vector: the length times 2^14 is the root of n 2^28, at most 2^25.
            const int64_t vl = g->l[f] - g->l[e], vc = g->c[f] - g->c[e];
            const int64_t len = static_cast<int64_t>(tone_graph_isqrt(static_cast<uint64_t>(vl * vl + vc * vc) << 28));
            if(len == 0) return false;
            g->ul[k0 + j] = static_cast<int16_t>(tone_graph_div_round(vl << 28, len));
            g->uc[k0 + j] = static_cast<int16_t>(tone_graph_div_round(vc << 28, len));
        }
    }
    if(n_top < 2 || n_top > TONE_GRAPH_TOP_MAX || top[0] != 0 || top[2 * n_top - 2] != TONE_GRAPH_ONE) return false;
    g->top_n = n_top;
    for(int i = 0; i < n_top; i++)
    {
        g->top_l[i] = top[2 * i], g->top_c[i] = top[2 * i + 1], g->top_s[i] = 0;
        if(i == 0) continue;
        const int dl = top[2 * i] - top[2 * i - 2];
        if(dl < 0) return false;
        // The floor of the slope in q16, at most 2^28 in size.
        const int64_t dc = static_cast<int64_t>(top[2 * i + 1] - top[2 * i - 1]) * 65536;
        if(dl > 0) g->top_s[i - 1] = static_cast<int32_t>(dc >= 0 ? dc / dl : -((-dc + dl - 1) / dl));
    }
    // The tone at each corner of the top, and the turn of each edge of it seen from the focal point.
    for(int i = 0; i < n_top; i++)
    {
        g->top_e[i] = TONE_GRAPH_END;
        for(unsigned e = 0; e < N && g->top_e[i] == TONE_GRAPH_END; e++)
            if(g->l[e] == g->top_l[i] && g->c[e] == g->top_c[i]) g->top_e[i] = static_cast<uint8_t>(e);
        if(g->top_e[i] == TONE_GRAPH_END) return false;
        if(i == 0) continue;
        const unsigned a = g->top_e[i - 1], b = g->top_e[i];
        bool           nb = false;
        for(unsigned k = g->at[a]; k < g->at[a + 1]; k++) nb = nb || g->nb[k] == b;
        const int32_t turn = (g->l[a] - TONE_GRAPH_FOCAL_L) * (g->c[b] - TONE_GRAPH_FOCAL_C) -
                             (g->c[a] - TONE_GRAPH_FOCAL_C) * (g->l[b] - TONE_GRAPH_FOCAL_L);
        if(!nb || turn >= 0) return false;
    }
    // The rim, by rising rim_lo.
    g->rim_n = 0;
    for(unsigned e = 0; e < N; e++)
        for(unsigned k = g->at[e]; k < g->at[e + 1]; k++)
        {
            const unsigned f = g->nb[k];
            if(f < e) continue;
            const int up = tone_graph_third(*g, e, f, 1), dn = tone_graph_third(*g, e, f, -1);
            if(up >= 0 && dn >= 0) continue;
            if(g->rim_n == TONE_GRAPH_RIM_MAX) return false;
            int j = g->rim_n++;
            const int16_t lo = g->l[e] < g->l[f] ? g->l[e] : g->l[f], hi = g->l[e] < g->l[f] ? g->l[f] : g->l[e];
            for(; j > 0 && g->rim_lo[j - 1] > lo; j--)
            {
                g->rim_a[j] = g->rim_a[j - 1], g->rim_b[j] = g->rim_b[j - 1], g->rim_t[j] = g->rim_t[j - 1];
                g->rim_ul[j] = g->rim_ul[j - 1], g->rim_uc[j] = g->rim_uc[j - 1];
                g->rim_lo[j] = g->rim_lo[j - 1], g->rim_hi[j] = g->rim_hi[j - 1];
            }
            g->rim_a[j] = static_cast<uint8_t>(e), g->rim_b[j] = static_cast<uint8_t>(f);
            g->rim_t[j]  = static_cast<uint8_t>(up >= 0 ? up : dn >= 0 ? dn : TONE_GRAPH_END);
            g->rim_ul[j] = g->ul[k], g->rim_uc[j] = g->uc[k], g->rim_lo[j] = lo, g->rim_hi[j] = hi;
        }
    g->cell = cell;
    g->cand = cand;
    g->rows = rows;
    return true;
}

// The owner of the point (l, c), q12. Exact, with ties to the lower entry, for every l from 0 to
// 4096 and every c below rows << TONE_GRAPH_CELL_SHIFT, which holds the whole area. A point outside
// takes the nearest of the candidates of the cell it is held to. At most 15 candidates, each one
// distance of 32 bits.
inline unsigned tone_graph_owner(const tone_graph& g, int l, int c)
{
    const int      i     = l < 0 ? 0 : l >> TONE_GRAPH_CELL_SHIFT > TONE_GRAPH_COLS - 1 ? TONE_GRAPH_COLS - 1
                                                                                         : l >> TONE_GRAPH_CELL_SHIFT;
    const int      j     = c < 0 ? 0 : c >> TONE_GRAPH_CELL_SHIFT > g.rows - 1 ? g.rows - 1 : c >> TONE_GRAPH_CELL_SHIFT;
    const unsigned w     = g.cell[i * g.rows + j];
    const uint8_t* p     = g.cand + (w >> 4);
    const bool     strip = c < TONE_GRAPH_STRIP;
    unsigned       best  = p[0];
    int32_t        bd    = INT32_MAX;
    for(unsigned k = 0; k < (w & 15); k++)
    {
        const unsigned e = p[k];
        if(strip && !g.neutral[e]) continue;
        const int32_t dl = g.l[e] - l, dc = g.c[e] - c;
        const int32_t d  = dl * dl + dc * dc;
        if(d < bd) bd = d, best = e;
    }
    return best;
}

// The neighbour of a at the least angle from (dl, dc), of those under 90 degrees, or -1 for none: the
// largest dot product of (dl, dc) with the unit vector toward the neighbour, the first of the ring on
// a tie. The unit vectors are rounded to 1 / TONE_GRAPH_UNIT, so two neighbours whose angles differ by
// less than about 1e-4 radians may come in either order. Each product stays below 2^27 for (dl, dc)
// within the plane. A coloured target lies at c of TONE_GRAPH_STRIP or above, so the strip rule of
// the owner only shapes the graph: across the strip line a coloured tone has the neutral tones whose
// bands its region borders as neighbours, and the angle weighs them like any other neighbour.
inline int tone_graph_toward(const tone_graph& g, unsigned a, int dl, int dc)
{
    int     best = -1;
    int32_t bd   = 0;
    for(unsigned k = g.at[a]; k < g.at[a + 1]; k++)
    {
        const int32_t d = g.ul[k] * dl + g.uc[k] * dc;
        if(d > bd) bd = d, best = g.nb[k];
    }
    return best;
}

// The highest c that a triangle of the graph reaches at l, 0 where none does, interpolated between
// the corners of the top and rounded down. At a step the higher side counts. It lies at most 2 below
// the exact value, so a point more than 2 above it lies in no triangle. l is held to 0 to
// TONE_GRAPH_ONE. A binary search finds the last corner at or before l.
inline int tone_graph_top(const tone_graph& g, int l)
{
    l      = l < 0 ? 0 : l > TONE_GRAPH_ONE ? TONE_GRAPH_ONE : l;
    int lo = 0, hi = g.top_n - 1;
    while(lo < hi)
    {
        const int mid = (lo + hi + 1) >> 1;
        if(g.top_l[mid] <= l) lo = mid;
        else hi = mid - 1;
    }
    int c;
    if(l == g.top_l[lo])
    {
        c = g.top_c[lo];
        if(lo > 0 && g.top_l[lo - 1] == l && g.top_c[lo - 1] > c) c = g.top_c[lo - 1];
    }
    else
        c = g.top_c[lo] + ((g.top_s[lo] * (l - g.top_l[lo])) >> 16);
    return c;
}

// The point of the rim nearest (l, c), for a point outside the triangles: rim pair *k and the parts
// w[0] on rim_a and w[1] on rim_b, one of them 0 at a corner. Each distance is compared squared in
// quarter steps, 32 bits: along the pair from the cross product with its unit vector, at a corner
// from the offset. A pair whose l lies farther from l than the best distance so far is skipped, and
// the scan stops at the first pair that begins beyond it. Exact to a quarter step.
inline void tone_graph_rim(const tone_graph& g, int l, int c, int* k, int32_t w[2])
{
    uint32_t best = UINT32_MAX;
    *k = 0, w[0] = 1, w[1] = 0;
    for(int i = 0; i < g.rim_n; i++)
    {
        const int32_t gap = g.rim_lo[i] - l > 0 ? g.rim_lo[i] - l : l - g.rim_hi[i] > 0 ? l - g.rim_hi[i] : 0;
        if(static_cast<uint32_t>(gap * gap) << 4 >= best)
        {
            if(g.rim_lo[i] > l) break;
            continue;
        }
        const unsigned a = g.rim_a[i], b = g.rim_b[i];
        const int32_t  el = l - g.l[a], ec = c - g.c[a], vl = g.l[b] - g.l[a], vc = g.c[b] - g.c[a];
        const int32_t  dot = el * vl + ec * vc, n = vl * vl + vc * vc;
        uint32_t       d;
        int32_t        wa = 1, wb = 0;
        if(dot <= 0) d = static_cast<uint32_t>(el * el + ec * ec) << 4;
        else if(dot >= n)
        {
            const int32_t fl = l - g.l[b], fc = c - g.c[b];
            d = static_cast<uint32_t>(fl * fl + fc * fc) << 4, wa = 0, wb = 1;
        }
        else
        {
            const int32_t  x = g.rim_ul[i] * ec - g.rim_uc[i] * el;
            const uint32_t q = static_cast<uint32_t>(x < 0 ? -x : x) >> 12;
            d = q * q, wa = n - dot, wb = dot;
        }
        if(d < best) best = d, *k = i, w[0] = wa, w[1] = wb;
    }
}

// The focal rule for a point (l, c) above the top: the point where the segment from it to the focal
// point crosses the top, its edge from corner *k to *k + 1, and the parts w[0] and w[1] of those two
// corners. The
// corners lie by falling angle seen from the focal point, so the sign of the cross product of the
// direction of (l, c) with each corner changes once along the top, and a binary search finds it. Each
// product stays below 2^25, 32 bits. A point above the top moves its crossing without a step, and a
// point on the top crosses at itself.
inline void tone_graph_focal(const tone_graph& g, int l, int c, int* k, int32_t w[2])
{
    const int32_t dl = l - TONE_GRAPH_FOCAL_L, dc = c - TONE_GRAPH_FOCAL_C;
    auto          cross = [&](int i) { return dl * (g.top_c[i] - TONE_GRAPH_FOCAL_C) - dc * (g.top_l[i] - TONE_GRAPH_FOCAL_L); };
    int           lo = 0, hi = g.top_n - 1;
    while(hi - lo > 1)
    {
        const int mid = (lo + hi) >> 1;
        if(cross(mid) >= 0) lo = mid;
        else hi = mid;
    }
    const int32_t a = cross(lo), b = cross(hi);
    *k = lo, w[0] = b < 0 ? -b : b, w[1] = a < 0 ? -a : a;
    if(w[0] == 0 && w[1] == 0) w[0] = 1;
}

// The unit vector from tone a to its neighbour b into ul, uc. False when b is no neighbour of a.
inline bool tone_graph_unit(const tone_graph& g, unsigned a, unsigned b, int* ul, int* uc)
{
    for(unsigned k = g.at[a]; k < g.at[a + 1]; k++)
        if(g.nb[k] == b)
        {
            *ul = g.ul[k], *uc = g.uc[k];
            return true;
        }
    return false;
}

// The tone that forms a triangle with a and b and lies on the left of a to b for side above 0, on
// the right for side below 0, or -1. The two triangles of a and b follow and precede b in a's ring.
inline int tone_graph_third(const tone_graph& g, unsigned a, unsigned b, int side)
{
    const unsigned k0 = g.at[a], k1 = g.at[a + 1];
    const int32_t  bl = g.l[b] - g.l[a], bc = g.c[b] - g.c[a];
    for(unsigned k = k0; k < k1; k++)
    {
        if(g.nb[k] != b) continue;
        const unsigned kn = k + 1 == k1 ? k0 : k + 1, kp = k == k0 ? k1 - 1 : k - 1;
        const int      cand[2] = {g.fl[k] & TONE_GRAPH_TRI ? g.nb[kn] : -1, g.fl[kp] & TONE_GRAPH_TRI ? g.nb[kp] : -1};
        for(int t : cand)
        {
            if(t < 0) continue;
            const int32_t cl = g.l[t] - g.l[a], cc = g.c[t] - g.c[a];
            const int32_t s  = bl * cc - bc * cl;
            if((side > 0 && s > 0) || (side < 0 && s < 0)) return t;
        }
    }
    return -1;
}
