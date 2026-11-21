// The ordered dither of photo pixels: the entries that enclose a coloured target and the pick of one
// of them by a threshold, so that over an area the picks average to the target in linear light.
// The triangles, the entries' coordinates and the start grid are data built by host_tests/tone_tables.
#pragma once

#include <cstdint>

static const uint8_t TONE_TRI_NONE   = 0xff;
static const int     TONE_PHOTO_GRID = 32;
static const int     TONE_PHOTO_V_MIN = -8192;
static const int     TONE_PHOTO_V_MAX = 24575;
static const int     TONE_PHOTO_WALK  = 64;

struct tone_tri
{
    uint8_t v[3];
    uint8_t nb[3];
    int8_t  sign;
    uint8_t pad;
};

// The plane of the mix: Y the luminance, V red over the mean of green and blue. Both are linear in
// linear light, so a mix of entries whose Y and V average to the target's matches it there, which
// is what the eye averages. A neutral has V 0. With these limits every difference of two points
// is below 2^15, so a cross product fits 32 bits.
inline void tone_photo_yv(int r, int g, int b, int* y, int* v)
{
    const int yy = (6966 * r + 23436 * g + 2366 * b + (1 << 14)) >> 15;
    const int vv = r - ((g + b) >> 1);
    *y           = yy < 0 ? 0 : yy > 32767 ? 32767 : yy;
    *v           = vv < TONE_PHOTO_V_MIN ? TONE_PHOTO_V_MIN : vv > TONE_PHOTO_V_MAX ? TONE_PHOTO_V_MAX : vv;
}

// v held to the range of the entries at luminance y: at or above the neutral axis and at or below
// the upper hull, whose entries top[0] to top[n - 1] run from black to paper by rising Y. The
// squeeze bounds chroma in Oklab, and a mix in linear light reaches less, so a target above the
// hull keeps its Y and loses V. yv holds Y and V of entry e at 2 e.
inline int tone_photo_clamp(const uint8_t* top, int n, const int16_t* yv, int y, int v)
{
    if(v <= 0) return 0;
    int k = 1;
    while(k < n - 1 && yv[2 * top[k]] < y) k++;
    const int16_t* a  = yv + 2 * top[k - 1];
    const int16_t* b  = yv + 2 * top[k];
    const int      dy = b[0] - a[0];
    const int64_t  up = static_cast<int64_t>(b[1] - a[1]) * (y - a[0]);
    if(static_cast<int64_t>(v - a[1]) * dy <= up) return v;
    int64_t q = up / dy;
    if(up % dy != 0 && up < 0) q--;
    return a[1] + static_cast<int>(q);
}

// The cell of the start grid for a target of squeezed L and chroma C, both q15.
inline int tone_photo_cell(int l, int c)
{
    const int i = l < 0 ? 0 : l >> 10 > TONE_PHOTO_GRID - 1 ? TONE_PHOTO_GRID - 1 : l >> 10;
    const int j = c < 0 ? 0 : c >> 8 > TONE_PHOTO_GRID - 1 ? TONE_PHOTO_GRID - 1 : c >> 8;
    return i * TONE_PHOTO_GRID + j;
}

// The point (y, v) against triangle t: w[i] is twice the area of the point and the edge opposite
// v[i], positive inside, so w[i] / (w[0] + w[1] + w[2]) is the weight of v[i].
inline void tone_photo_area(const tone_tri& t, const int16_t* yv, int y, int v, int32_t w[3])
{
    for(int i = 0; i < 3; i++)
    {
        const int16_t* a = yv + 2 * t.v[i == 2 ? 0 : i + 1];
        const int16_t* b = yv + 2 * t.v[i == 0 ? 2 : i - 1];
        w[i] = t.sign * ((a[0] - y) * (b[1] - v) - (a[1] - v) * (b[0] - y));
    }
}

// The triangle that holds (y, v), walked from triangle k across the edge it is furthest outside.
// tone_photo_clamp() keeps a target in the range. A point outside it would stop at its edge with
// the negative weights dropped.
inline unsigned tone_photo_find(const tone_tri* tri, const int16_t* yv, unsigned k, int y, int v, int32_t w[3])
{
    for(int step = 0;; step++)
    {
        const tone_tri& t = tri[k];
        tone_photo_area(t, yv, y, v, w);
        int out = -1;
        for(int i = 0; i < 3; i++)
            if(w[i] < 0 && t.nb[i] != TONE_TRI_NONE && (out < 0 || w[i] < w[out])) out = i;
        if(out < 0 || step == TONE_PHOTO_WALK) break;
        k = t.nb[out];
    }
    for(int i = 0; i < 3; i++)
        if(w[i] < 0) w[i] = 0;
    return k;
}

// Of the entries of triangle t with weights w, the one at threshold rank r, 0 to 255: the darkest
// for the lowest ranks. Exact, so the share of each entry over the 256 ranks is its weight to
// within 1/512.
inline unsigned tone_photo_choose(const tone_tri& t, const int32_t w[3], int r)
{
    const int64_t s  = static_cast<int64_t>(w[0]) + w[1] + w[2];
    const int64_t th = (2 * r + 1) * s;
    if(th < static_cast<int64_t>(w[0]) * 512) return t.v[0];
    if(th < (static_cast<int64_t>(w[0]) + w[1]) * 512) return t.v[1];
    return t.v[2];
}

// A neutral target of linear light lin between the neutral entries lo and hi (darker first), of
// linear light ll and lh, at threshold rank r.
inline unsigned tone_photo_pair(unsigned lo, unsigned hi, int ll, int lh, int lin, int r)
{
    const int v = lin < ll ? ll : lin > lh ? lh : lin;
    return (2 * r + 1) * (lh - ll) < 512 * (lh - v) ? lo : hi;
}

// The nearest mix of a coloured photo pixel (TONE_PHOTO_NEAREST of tones.h), in the plane of L and
// 2C of tone_graph.h. The graph G of that plane answers (tones.cpp maps these to tone_graph.h):
//   G::owner(l, c)        the entry that owns the point (l, c)
//   G::toward(a, dl, dc)  the neighbour of a in the direction (dl, dc), or -1
//   G::third(a, b, side)  the entry that forms a triangle with a and b on the left of a to b for side
//                         above 0, on the right for side below 0, or -1
//   G::l(e), G::c(e)      the point of entry e
// So a mix holds only the owner, its neighbours and the entries of their triangles. A target takes:
// 1. A, the owner of T, the target.
// 2. When T lies more than TONE_NEAR_ONE_Q from A: B, the entry toward T - A from A. The mix of A
//    and B reaches P, the point of the segment A B nearest T.
// 3. When T lies more than TONE_NEAR_TWO_Q from P: C, the third entry of A and B on the side of
//    A B that T is on. The mix of A, B and C reaches the point of the triangle nearest T, which is
//    T when the triangle holds it.
// Without B the target takes A alone, and without C it takes the mix of A and B. The weights are the
// position of that point among the entries in the plane.
static const int TONE_NEAR_ONE_Q = 8;
static const int TONE_NEAR_TWO_Q = 8;

// How tone_near_pick() ended, for the host checks.
enum tone_near_how : uint8_t
{
    TONE_NEAR_ON,
    TONE_NEAR_NO_B,
    TONE_NEAR_LINE,
    TONE_NEAR_NO_C,
    TONE_NEAR_IN,
    TONE_NEAR_EDGE_OF,
};

// The entries of a mix, darkest first, and their weights. Entries past n have weight 0.
struct tone_near_mix
{
    uint8_t       n;
    tone_near_how how;
    uint8_t       v[3];
    int64_t       w[3];
};


// The weights of the point of triangle a b c nearest p, all as offsets from a, into w. Exact: the
// cases of the regions of a triangle's vertices, edges and inside. False for a flat triangle.
inline bool tone_near_triangle(int64_t bl, int64_t bc, int64_t cl, int64_t cc, int64_t pl, int64_t pc, int64_t w[3])
{
    const int64_t d1 = bl * pl + bc * pc, d2 = cl * pl + cc * pc;
    if(d1 <= 0 && d2 <= 0) return w[0] = 1, w[1] = w[2] = 0, true;
    const int64_t d3 = d1 - (bl * bl + bc * bc), d4 = d2 - (cl * bl + cc * bc);
    if(d3 >= 0 && d4 <= d3) return w[1] = 1, w[0] = w[2] = 0, true;
    const int64_t vc = d1 * d4 - d3 * d2;
    if(vc <= 0 && d1 >= 0 && d3 <= 0) return w[0] = -d3, w[1] = d1, w[2] = 0, true;
    const int64_t d5 = d1 - (bl * cl + bc * cc), d6 = d2 - (cl * cl + cc * cc);
    if(d6 >= 0 && d5 <= d6) return w[2] = 1, w[0] = w[1] = 0, true;
    const int64_t vb = d5 * d2 - d1 * d6;
    if(vb <= 0 && d2 >= 0 && d6 <= 0) return w[0] = -d6, w[1] = 0, w[2] = d2, true;
    const int64_t va = d3 * d6 - d5 * d4;
    if(va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return w[0] = 0, w[1] = d5 - d6, w[2] = d4 - d3, true;
    w[0] = va, w[1] = vb, w[2] = vc;
    return va + vb + vc > 0;
}

template <class G> void tone_near_sort(const G& g, tone_near_mix* m)
{
    for(int i = 1; i < m->n; i++)
        for(int j = i; j > 0; j--)
        {
            const unsigned x = m->v[j - 1], y = m->v[j];
            if(g.l(x) < g.l(y) || (g.l(x) == g.l(y) && x < y)) break;
            m->v[j - 1] = static_cast<uint8_t>(y), m->v[j] = static_cast<uint8_t>(x);
            const int64_t t = m->w[j - 1];
            m->w[j - 1] = m->w[j], m->w[j] = t;
        }
}

// The mix of the target at L l and 2C c, q12. Every product stays below 2^50 while the points of
// the plane lie within 0 to 4096.
template <class G> void tone_near_pick(const G& g, int l, int c, tone_near_mix* m)
{
    const unsigned a  = g.owner(l, c);
    const int32_t  al = g.l(a), ac = g.c(a);
    const int32_t  el = l - al, ec = c - ac;
    m->n = 1, m->how = TONE_NEAR_ON;
    m->v[0] = m->v[1] = m->v[2] = static_cast<uint8_t>(a);
    m->w[0] = 1, m->w[1] = m->w[2] = 0;
    const int32_t ee = el * el + ec * ec;
    if(ee <= TONE_NEAR_ONE_Q * TONE_NEAR_ONE_Q) return;

    const int bi = g.toward(a, el, ec);
    if(bi < 0)
    {
        m->how = TONE_NEAR_NO_B;
        return;
    }
    const unsigned b   = static_cast<unsigned>(bi);
    const int32_t  bl  = g.l(b) - al, bc = g.c(b) - ac;
    const int64_t  d11 = bl * bl + bc * bc;
    const int64_t  eb  = bl * el + bc * ec;
    const int64_t  t   = eb < 0 ? 0 : eb < d11 ? eb : d11;
    m->n = 2, m->how = TONE_NEAR_LINE;
    m->v[1] = m->v[2] = static_cast<uint8_t>(b);
    m->w[0] = d11 - t, m->w[1] = t;
    // The square of T - P, times d11.
    const int32_t fl = l - g.l(b), fc = c - g.c(b);
    const int64_t r2 = t == 0     ? static_cast<int64_t>(ee) * d11
                       : t == d11 ? static_cast<int64_t>(fl * fl + fc * fc) * d11
                                  : static_cast<int64_t>(ee) * d11 - t * t;
    const int64_t side = static_cast<int64_t>(bl) * ec - static_cast<int64_t>(bc) * el;
    if(r2 > static_cast<int64_t>(TONE_NEAR_TWO_Q * TONE_NEAR_TWO_Q) * d11)
    {
        m->how       = TONE_NEAR_NO_C;
        const int ci = side == 0 ? -1 : g.third(a, b, side > 0 ? 1 : -1);
        int64_t      w[3];
        if(ci >= 0 && tone_near_triangle(bl, bc, g.l(static_cast<unsigned>(ci)) - al,
                                         g.c(static_cast<unsigned>(ci)) - ac, el, ec, w))
        {
            const bool in = w[0] > 0 && w[1] > 0 && w[2] > 0;
            m->n = 3, m->how = in ? TONE_NEAR_IN : TONE_NEAR_EDGE_OF;
            m->v[2] = static_cast<uint8_t>(ci);
            m->w[0] = w[0], m->w[1] = w[1], m->w[2] = w[2];
        }
    }
    tone_near_sort(g, m);
}

// Of the mix m, the entry at threshold rank r, 0 to 255: the darkest for the lowest ranks. Exact,
// so the share of each entry over the 256 ranks is its weight to within 1/512.
inline unsigned tone_near_choose(const tone_near_mix& m, int r)
{
    const int64_t s  = m.w[0] + m.w[1] + m.w[2];
    const int64_t th = (2 * r + 1) * s;
    if(th < m.w[0] * 512) return m.v[0];
    if(th < (m.w[0] + m.w[1]) * 512) return m.v[1];
    return m.v[2];
}
