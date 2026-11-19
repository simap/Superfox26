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
