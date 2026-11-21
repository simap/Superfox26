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
struct tone_graph
{
    int16_t         l[TONEMAP_COUNT];
    int16_t         c[TONEMAP_COUNT];
    uint8_t         neutral[TONEMAP_COUNT];
    uint16_t        at[TONEMAP_COUNT + 1];
    uint8_t         nb[TONE_GRAPH_SLOTS];
    uint8_t         fl[TONE_GRAPH_SLOTS];
    const uint16_t* cell;
    const uint8_t*  cand;
    int             rows;
};

// Fills g from the tables of main/tone_tables.inc: qL and graph_c per entry, the neutral entries,
// the rings and the owner grid of rows rows. cell and cand stay where they are and must stay valid.
// False when a ring runs past the end of ring or past TONE_GRAPH_SLOTS, or names no entry.
inline bool tone_graph_hydrate(tone_graph* g, const int16_t* qL, const int16_t* gc, const uint8_t* neutral, int n_neutral,
                               const uint8_t* ring, int n_ring, const uint16_t* cell, const uint8_t* cand, int rows)
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
            if(v >= N || k >= static_cast<unsigned>(TONE_GRAPH_SLOTS)) return false;
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
        }
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

// The neighbour of a whose border the ray from a along (dl, dc) crosses first, of the neighbours at
// under 90 degrees from (dl, dc), or -1 for none. The ray meets the border with neighbour f at
// t = |f - a|^2 / (2 (f - a).(dl, dc)) and the least t wins. On equal t the ray passes a corner and
// enters the region of the larger (f - a).(dl, dc), then of the lower entry. Below the strip only
// neutral tones own, so a coloured a whose ray reaches c = TONE_GRAPH_STRIP no later than any such
// border takes the neutral neighbour nearest in l to that point, on equal distance the one the ray
// moves toward in l, then the lower entry, if it lies under 90 degrees, else none. A neutral a with dc at or below 0 takes neutral neighbours
// only. For (dl, dc) = T - a with T a target of a's region, the result is the region the ray enters
// first past T, when the ray meets it inside the area. Products stay below 2^52.
inline int tone_graph_toward(const tone_graph& g, unsigned a, int dl, int dc)
{
    const int32_t  al = g.l[a], ac = g.c[a];
    const bool     axis = g.neutral[a] && dc <= 0;
    int            best = -1;
    int64_t        bn = 0, bd = 1;
    for(unsigned k = g.at[a]; k < g.at[a + 1]; k++)
    {
        const unsigned f = g.nb[k];
        if(axis && !g.neutral[f]) continue;
        const int32_t ul = g.l[f] - al, uc = g.c[f] - ac;
        const int64_t dot = static_cast<int64_t>(ul) * dl + static_cast<int64_t>(uc) * dc;
        if(dot <= 0) continue;
        const int64_t n  = static_cast<int64_t>(ul) * ul + static_cast<int64_t>(uc) * uc;
        const int64_t lt = n * bd, rt = bn * dot;
        if(best < 0 || lt < rt || (lt == rt && (dot > bd || (dot == bd && static_cast<int>(f) < best))))
            best = static_cast<int>(f), bn = n, bd = dot;
    }
    if(g.neutral[a] || dc >= 0) return best;
    // The ray reaches the strip at t = h / -dc. It wins when t <= bn / (2 bd).
    const int64_t h = ac - TONE_GRAPH_STRIP;
    if(best >= 0 && 2 * h * bd > bn * -static_cast<int64_t>(dc)) return best;
    // The point where it does, times -dc: its l.
    const int64_t xs = static_cast<int64_t>(al) * -dc + h * dl;
    int           nb = -1;
    int64_t       nd = 0;
    for(unsigned k = g.at[a]; k < g.at[a + 1]; k++)
    {
        const unsigned f = g.nb[k];
        if(!g.neutral[f]) continue;
        const int64_t x = static_cast<int64_t>(g.l[f]) * -dc - xs;
        const int64_t d = x < 0 ? -x : x;
        if(nb < 0 || d < nd || (d == nd && (x * dl > 0 || (x * dl == 0 && static_cast<int>(f) < nb))))
            nb = static_cast<int>(f), nd = d;
    }
    if(nb < 0) return -1;
    const int64_t dot = static_cast<int64_t>(g.l[nb] - al) * dl + static_cast<int64_t>(g.c[nb] - ac) * dc;
    return dot > 0 ? nb : -1;
}

// The tone that forms a triangle with a and b and lies on the left of a to b for side above 0, on
// the right for side below 0, or -1. The two triangles of a and b follow and precede b in a's ring.
inline int tone_graph_third(const tone_graph& g, unsigned a, unsigned b, int side)
{
    const unsigned k0 = g.at[a], deg = g.at[a + 1] - k0;
    const int32_t  bl = g.l[b] - g.l[a], bc = g.c[b] - g.c[a];
    for(unsigned j = 0; j < deg; j++)
    {
        if(g.nb[k0 + j] != b) continue;
        const unsigned jn = (j + 1) % deg, jp = (j + deg - 1) % deg;
        const int      cand[2] = {g.fl[k0 + j] & TONE_GRAPH_TRI ? g.nb[k0 + jn] : -1,
                                  g.fl[k0 + jp] & TONE_GRAPH_TRI ? g.nb[k0 + jp] : -1};
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
