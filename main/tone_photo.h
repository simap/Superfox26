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
//   G::top(l)             the highest c that a triangle of the graph reaches at l, at most 2 low, so
//                         a point more than 2 above it lies in no triangle
//   G::owner(l, c)        the entry that owns the point (l, c)
//   G::toward(a, dl, dc)  the neighbour of a at the least angle from (dl, dc), under 90 degrees, or -1
//   G::third(a, b, side)  the entry that forms a triangle with a and b on the left of a to b for side
//                         above 0, on the right for side below 0, or -1
//   G::focal(l, c, t, w)  the focal rule for a point above the top: the point where the segment from
//                         it to the focal point crosses the top, on the pair t[0] t[1] with the parts
//                         w[0] and w[1], t[2] t[1], w[2] 0
//   G::rim(l, c, t, w)    the closest rule, the same with the point nearest it of the union of the
//                         triangles and the pairs in no triangle, t[2] the third corner of its triangle
//                         or t[1]
//   G::unit(a, b, ul, uc) the vector from a to its neighbour b at the length 2^14, false for none
//   G::l(e), G::c(e)      the point of entry e
// A target T at (l, c) keeps its l and c and takes:
// 1. When T lies at most 2 above top(l): A, the owner of T, B, the neighbour of A toward T, and C,
//    the third entry of A and B on the side of A B that T is on, or on the other side when that side
//    has none. When the point of triangle A B C nearest T lies inside an edge and T beyond it, the
//    triangle across the edge takes the place of A B C, up to TONE_NEAR_STEPS times. When the
//    triangle holds T, the weights are T's position in it. A target on its owner takes the owner.
// 2. Else, or when no triangle on the way holds T, the focal rule: the point where the segment from
//    T to the focal point of tone_graph.h crosses the top, as the parts of the two tones of that
//    edge. Each point of the top takes the targets of one direction, so the mix moves with the
//    target without a step, and a target on the top takes itself, the mix of step 1.
// 3. The ease of tone_near_ease.
// Every two entries of a mix with weight are neighbours, and a target that a triangle holds is met
// to the rounding of the weights before the ease.
static const int TONE_NEAR_STEPS = 3;
// The weights of a mix sum to below 2^TONE_NEAR_BITS, and to 2^14 or more but for a mix of one.
static const int TONE_NEAR_BITS = 15;

// How tone_near_pick() ended.
enum tone_near_how : uint8_t
{
    TONE_NEAR_IN,  // a triangle holds T, or T lies on its owner
    TONE_NEAR_RIM, // T lies outside the triangles, and the mix is a point of the top or the rim
};

// The entries of a mix, darkest first, their weights and the steps across edges. The entries are
// the corners of a triangle, but for a pair in no triangle and a target on its owner, where an entry
// of weight 0 repeats another.
struct tone_near_mix
{
    uint8_t       v[3];
    tone_near_how how;
    uint8_t       steps;
    int16_t       w[3];
};

// A step of the ease: smoothstep((e - t) / w) of a distance e in q12 units, 0 at e <= t and 1 at
// e >= t + w. t 0 and w 0 turn it off, w 0 alone makes a hard step at t. r is 2^15 / w, rounded
// down, at most 32767, so the factor is a cubic in q15.
struct tone_near_step
{
    int t = 0, w = 0, r = 0;
};

inline tone_near_step tone_near_step_of(int t, int w)
{
    tone_near_step z;
    z.t = t < 0 ? 0 : t, z.w = w < 0 ? 0 : w;
    z.r = z.w > 0 ? (32768 / z.w > 32767 ? 32767 : 32768 / z.w) : 0;
    return z;
}

inline bool tone_near_step_on(const tone_near_step& z)
{
    return z.t > 0 || z.w > 0;
}

// The factor at the distance e, q15 with 32768 for 1: 3 x^2 - 2 x^3 as x2 + 2 (x2 - x3), each term
// below 2^15.
inline int32_t tone_near_step_at(const tone_near_step& z, int e)
{
    const int d = e - z.t;
    if(d <= 0) return 0;
    if(d >= z.w) return 32768;
    const int32_t x = d * z.r, x2 = (x * x) >> 15, x3 = (x2 * x) >> 15;
    return x2 + 2 * (x2 - x3);
}

// The ease of the minor tones of a mix, two steps, each a blend w = lerp(w_near, w, step(distance)):
//   line   for a mix in a triangle, the edge nearest T by the distance e to its nearest point, and
//          w_near the two parts of that point. So a target within t of an edge takes the two tones
//          of that edge alone, and the mix point moves by at most e. At most one corner drops. It
//          draws a band of another texture along every edge, so it is off by default.
//   solid  d, the distance from the corner of the largest weight after the line step to the point
//          the mix reaches, T in a triangle and the point of the top or rim outside, and w_near that
//          corner alone. So a target within t of a tone takes that tone alone, in a disc
//          around each tone point, and the mix point moves by at most d. Nothing changes along the
//          edges.
// Both steps off give the linear weights exactly, and the mix point moves by at most the larger
// reach t + w of the two. closest, for comparison, makes a target outside the triangles take the
// closest rule, the point of the rim nearest it, in place of the focal rule.
struct tone_near_ease
{
    tone_near_step solid, line;
    bool           closest = false;
};

inline tone_near_ease tone_near_ease_of(int solid_t, int solid_w, int line_t, int line_w)
{
    tone_near_ease z;
    z.solid = tone_near_step_of(solid_t, solid_w), z.line = tone_near_step_of(line_t, line_w);
    return z;
}

// The integer square root of v, rounded down.
inline uint32_t tone_near_isqrt(uint32_t v)
{
    uint32_t r = 0;
    for(uint32_t b = 1u << 30; b != 0; b >>= 2)
        if(v >= r + b)
        {
            v -= r + b;
            r = (r >> 1) + b;
        }
        else
            r >>= 1;
    return r;
}

// The point of triangle t nearest (l, c) as weights of its corners, into w. Returns -1 when the
// triangle holds (l, c), i when the point lies inside the edge opposite corner i and (l, c) beyond
// that edge, or 3 when the point is a corner and (l, c) lies outside. Inside, w[i] is twice the area
// of (l, c) and the edge opposite corner i. On an edge the weights are its two parts. Every product
// stays below 2^26 while the points lie in the plane, l from 0 to 4096 and c below 2048.
template <class G> int tone_near_area(const G& g, const uint8_t t[3], int l, int c, int32_t w[3])
{
    int32_t pl[3], pc[3];
    for(int i = 0; i < 3; i++) pl[i] = g.l(t[i]) - l, pc[i] = g.c(t[i]) - c;
    for(int i = 0; i < 3; i++)
    {
        const int j = i == 2 ? 0 : i + 1, k = i == 0 ? 2 : i - 1;
        w[i]        = pl[j] * pc[k] - pc[j] * pl[k];
    }
    if(w[0] + w[1] + w[2] < 0)
        for(int i = 0; i < 3; i++) w[i] = -w[i];
    if(w[0] >= 0 && w[1] >= 0 && w[2] >= 0) return -1;
    // Outside. The point lies inside edge j k when (l, c) projects into the edge from beyond it.
    for(int i = 0; i < 3; i++)
    {
        if(w[i] >= 0) continue;
        const int     j = i == 2 ? 0 : i + 1, k = i == 0 ? 2 : i - 1;
        const int32_t el = pl[k] - pl[j], ec = pc[k] - pc[j];
        const int32_t s = -(pl[j] * el + pc[j] * ec), d = el * el + ec * ec;
        if(s > 0 && s < d)
        {
            w[i] = 0, w[j] = d - s, w[k] = s;
            return i;
        }
    }
    int     n  = 0;
    int32_t nd = INT32_MAX;
    for(int i = 0; i < 3; i++)
    {
        const int32_t d = pl[i] * pl[i] + pc[i] * pc[i];
        if(d < nd) nd = d, n = i;
    }
    w[0] = w[1] = w[2] = 0;
    w[n] = 1;
    return 3;
}

// The entries t and their weights w, at least 0, of T at (l, c) into m: the weights to below
// 2^TONE_NEAR_BITS, all shifted by one amount, so each keeps 14 bits or more of the sum, then eased
// by z (how tells a triangle from a pair t[0] t[1]), and the entries darkest first, by l, then by
// entry. Each product stays below 2^31. The solid step reads the distance squared, below 2^26, and
// takes its root only within the reach t + w of the step.
template <class G>
void tone_near_put(const G& g, const tone_near_ease& z, const uint8_t t[3], const int32_t w[3], tone_near_how how, int l,
                   int c, tone_near_mix* m)
{
    const uint32_t sum = static_cast<uint32_t>(w[0]) + static_cast<uint32_t>(w[1]) + static_cast<uint32_t>(w[2]);
    const int      sh  = sum >> TONE_NEAR_BITS ? 32 - __builtin_clz(sum) - TONE_NEAR_BITS : 0;
    int32_t        n[3];
    for(int i = 0; i < 3; i++) n[i] = w[i] >> sh;
    if(tone_near_step_on(z.line) && how == TONE_NEAR_IN && t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
    {
        // The edge nearest T, by the distance to its nearest point, squared in quarter steps as in
        // tone_graph_rim(), each edge read from its lower entry and a tie to the edge whose far corner
        // is the lower entry, so that every order of the corners gives the same edge. x is the far
        // corner, j and k the ends, a and b the parts of the nearest point.
        int      x = -1, xj = 0, xk = 0;
        int32_t  xa = 0, xb = 0;
        uint32_t best = UINT32_MAX;
        for(int i = 0; i < 3; i++)
        {
            int j = i == 2 ? 0 : i + 1, k = i == 0 ? 2 : i - 1;
            if(t[k] < t[j])
            {
                const int s = j;
                j = k, k = s;
            }
            int ul, uc;
            if(!g.unit(t[j], t[k], &ul, &uc)) continue;
            const int32_t el = l - g.l(t[j]), ec = c - g.c(t[j]), vl = g.l(t[k]) - g.l(t[j]), vc = g.c(t[k]) - g.c(t[j]);
            const int32_t dot = el * vl + ec * vc, n2 = vl * vl + vc * vc;
            uint32_t      d2;
            int32_t       a = n2 - dot, b = dot;
            if(dot <= 0) d2 = static_cast<uint32_t>(el * el + ec * ec) << 4, a = 1, b = 0;
            else if(dot >= n2)
            {
                const int32_t fl = l - g.l(t[k]), fc = c - g.c(t[k]);
                d2 = static_cast<uint32_t>(fl * fl + fc * fc) << 4, a = 0, b = 1;
            }
            else
            {
                const int32_t  cr = ul * ec - uc * el;
                const uint32_t q  = static_cast<uint32_t>(cr < 0 ? -cr : cr) >> 12;
                d2                = q * q;
            }
            if(d2 < best || (d2 == best && t[i] < t[x])) best = d2, x = i, xj = j, xk = k, xa = a, xb = b;
        }
        const int32_t reach = z.line.t + z.line.w;
        if(x >= 0 && best < static_cast<uint32_t>(reach * reach) << 4)
        {
            // The parts of the edge to the sum of n, with both cut to below 2^16 first so the
            // products fit 32 bits, then w = lerp(edge, linear, step(e)).
            const int32_t f  = tone_near_step_at(z.line, static_cast<int>(tone_near_isqrt(best) >> 2));
            const int32_t s  = n[0] + n[1] + n[2];
            const int     cut = 32 - __builtin_clz(static_cast<uint32_t>(xa + xb)) > 16 ? 32 - __builtin_clz(static_cast<uint32_t>(xa + xb)) - 16 : 0;
            const int32_t pa = xa >> cut, pb = xb >> cut, den = pa + pb > 0 ? pa + pb : 1;
            int32_t       e[3];
            e[x]  = 0;
            e[xj] = s * pa / den;
            e[xk] = s - e[xj];
            for(int i = 0; i < 3; i++) n[i] = ((n[i] * f) >> 15) + ((e[i] * (32768 - f)) >> 15);
        }
    }
    if(tone_near_step_on(z.solid))
    {
        // The corner of the largest weight, the lower entry on a tie, and the distance from it of the
        // point the mix reaches: T in a triangle, else the point of the weights, each product below
        // 2^28.
        int b = 0;
        for(int i = 1; i < 3; i++)
            if(n[i] > n[b] || (n[i] == n[b] && t[i] < t[b])) b = i;
        int32_t pl = l, pc = c;
        if(how == TONE_NEAR_RIM)
        {
            const int32_t s = n[0] + n[1] + n[2] > 0 ? n[0] + n[1] + n[2] : 1;
            pl = pc = 0;
            for(int i = 0; i < 3; i++) pl += n[i] * g.l(t[i]), pc += n[i] * g.c(t[i]);
            pl = (pl + s / 2) / s, pc = (pc + s / 2) / s;
        }
        const int32_t  dl = pl - g.l(t[b]), dc = pc - g.c(t[b]), reach = z.solid.t + z.solid.w;
        const uint32_t d2 = static_cast<uint32_t>(dl * dl + dc * dc);
        if(d2 < static_cast<uint32_t>(reach * reach))
        {
            const int32_t f   = tone_near_step_at(z.solid, static_cast<int>(tone_near_isqrt(d2)));
            int32_t       all = 0, rest = 0;
            for(int i = 0; i < 3; i++)
            {
                all += n[i];
                if(i == b) continue;
                n[i] = (n[i] * f) >> 15;
                rest += n[i];
            }
            n[b] = all - rest;
        }
    }
    for(int i = 0; i < 3; i++) m->v[i] = t[i], m->w[i] = static_cast<int16_t>(n[i]);
    m->how = how;
    for(int i = 1; i < 3; i++)
        for(int j = i; j > 0; j--)
        {
            const unsigned x = m->v[j - 1], y = m->v[j];
            if(g.l(x) < g.l(y) || (g.l(x) == g.l(y) && x <= y)) break;
            m->v[j - 1] = static_cast<uint8_t>(y), m->v[j] = static_cast<uint8_t>(x);
            const int16_t k = m->w[j - 1];
            m->w[j - 1] = m->w[j], m->w[j] = k;
        }
}

// Step 1 for T at (l, c): true with the triangle t that holds T and T's weights in it, or with t the
// owner three times and w 1 0 0 for a target on its owner. *steps counts the steps across edges.
template <class G> bool tone_near_find(const G& g, int l, int c, uint8_t t[3], int32_t w[3], int* steps)
{
    const unsigned a  = g.owner(l, c);
    const int32_t  al = g.l(a), ac = g.c(a);
    *steps            = 0;
    t[0] = t[1] = t[2] = static_cast<uint8_t>(a);
    w[0] = 1, w[1] = w[2] = 0;
    if(l == al && c == ac) return true;
    const int bi = g.toward(a, l - al, c - ac);
    if(bi < 0) return false;
    const unsigned b  = static_cast<unsigned>(bi);
    const int32_t  bl = g.l(b) - al, bc = g.c(b) - ac;
    const int32_t  side = bl * (c - ac) - bc * (l - al);
    int            ci   = g.third(a, b, side >= 0 ? 1 : -1);
    if(ci < 0) ci = g.third(a, b, side >= 0 ? -1 : 1);
    if(ci < 0) return false;
    t[1] = static_cast<uint8_t>(b), t[2] = static_cast<uint8_t>(ci);
    for(;;)
    {
        const int x = tone_near_area(g, t, l, c, w);
        if(x < 0) return true;
        if(x > 2 || *steps == TONE_NEAR_STEPS) return false;
        // Across the edge p q: the triangle on the side of (l, c), which is not that of t[x].
        const unsigned p = t[x == 2 ? 0 : x + 1], q = t[x == 0 ? 2 : x - 1];
        const int32_t  s = (g.l(q) - g.l(p)) * (c - g.c(p)) - (g.c(q) - g.c(p)) * (l - g.l(p));
        const int      n = g.third(p, q, s > 0 ? 1 : -1);
        if(n < 0) return false;
        t[x] = static_cast<uint8_t>(n);
        ++*steps;
    }
}

// The mix of the target at L l and 2C c, q12, with the ease z.
template <class G> void tone_near_pick(const G& g, const tone_near_ease& z, int l, int c, tone_near_mix* m)
{
    uint8_t t[3];
    int32_t w[3];
    int     steps = 0;
    if(c <= g.top(l) + 2 && tone_near_find(g, l, c, t, w, &steps))
        tone_near_put(g, z, t, w, TONE_NEAR_IN, l, c, m);
    else
    {
        if(z.closest) g.rim(l, c, t, w);
        else g.focal(l, c, t, w);
        tone_near_put(g, z, t, w, TONE_NEAR_RIM, l, c, m);
    }
    m->steps = static_cast<uint8_t>(steps);
}

// The last triangle that held a target, for tone_near_pick_from().
struct tone_near_last
{
    uint8_t t[3];
    bool    ok = false;
};

// tone_near_pick() that first tries the triangle of last, and keeps the triangle that holds T in it.
// Adjacent pixels of a photo mostly fall in one triangle, so most targets skip step 1. A target on an
// edge or corner of that triangle takes tone_near_pick(), since a second triangle holds it too and
// the ease of the two differs there. So the mix is always that of tone_near_pick().
template <class G>
void tone_near_pick_from(const G& g, const tone_near_ease& z, tone_near_last* last, int l, int c, tone_near_mix* m)
{
    int32_t w[3];
    if(last->ok && tone_near_area(g, last->t, l, c, w) < 0 && w[0] > 0 && w[1] > 0 && w[2] > 0)
    {
        tone_near_put(g, z, last->t, w, TONE_NEAR_IN, l, c, m);
        m->steps = 0;
        return;
    }
    tone_near_pick(g, z, l, c, m);
    last->ok = m->how == TONE_NEAR_IN && m->v[0] != m->v[1] && m->v[1] != m->v[2] && m->v[0] != m->v[2];
    for(int i = 0; i < 3; i++) last->t[i] = m->v[i];
}

// Of the mix m, the entry at threshold rank r, 0 to 255: the darkest for the lowest ranks. Exact,
// so the share of each entry over the 256 ranks is its weight to within 1/512.
inline unsigned tone_near_choose(const tone_near_mix& m, int r)
{
    const int32_t s  = m.w[0] + m.w[1] + m.w[2];
    const int32_t th = (2 * r + 1) * s;
    if(th < m.w[0] * 512) return m.v[0];
    if(th < (m.w[0] + m.w[1]) * 512) return m.v[1];
    return m.v[2];
}
