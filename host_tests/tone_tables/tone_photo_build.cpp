// The tables of the photo dither (main/tone_photo.h): a Delaunay triangulation of the entries in
// the plane of Y and V, and the triangle a walk starts from per cell of target L and C.

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "tone_photo.h"
#include "tone_tables_build.h"

namespace
{
struct pt
{
    int64_t x, y;
};

int64_t orient(const pt& a, const pt& b, const pt& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Above 0 when d lies inside the circle through the counter clockwise triangle a, b, c. Exact.
bool in_circle(const pt& a, const pt& b, const pt& c, const pt& d)
{
    const __int128 adx = a.x - d.x, ady = a.y - d.y, bdx = b.x - d.x, bdy = b.y - d.y, cdx = c.x - d.x,
                   cdy = c.y - d.y;
    const __int128 ad = adx * adx + ady * ady, bd = bdx * bdx + bdy * bdy, cd = cdx * cdx + cdy * cdy;
    const __int128 det = adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) + ad * (bdx * cdy - bdy * cdx);
    return det > 0;
}

int64_t hull_area2(std::vector<pt> p)
{
    std::sort(p.begin(), p.end(), [](const pt& a, const pt& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<pt> h(2 * p.size());
    size_t          k = 0;
    for(size_t i = 0; i < p.size(); i++)
    {
        while(k >= 2 && orient(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    for(size_t i = p.size() - 1, t = k + 1; i-- > 0;)
    {
        while(k >= t && orient(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    int64_t a = 0;
    for(size_t i = 0; i + 1 < k; i++) a += h[i].x * h[i + 1].y - h[i + 1].x * h[i].y;
    return a;
}
} // namespace

bool tone_photo_build(const int16_t* yv, const bool* use, int n, std::vector<tone_tri>& out,
                      std::vector<uint8_t>& top, char* why, size_t len)
{
    std::vector<pt>  p;
    std::vector<int> ent;
    for(int e = 0; e < n; e++)
    {
        if(!use[e]) continue;
        bool dup = false;
        for(int f : ent) dup = dup || (yv[2 * f] == yv[2 * e] && yv[2 * f + 1] == yv[2 * e + 1]);
        if(dup) continue;
        p.push_back({yv[2 * e], yv[2 * e + 1]});
        ent.push_back(e);
    }
    const int np = static_cast<int>(p.size());
    if(np < 3)
    {
        snprintf(why, len, "%d entries to triangulate", np);
        return false;
    }
    const int64_t S = 4000000;
    p.push_back({-S, -S});
    p.push_back({S, -S});
    p.push_back({0, S});

    // Bowyer and Watson: each point removes the triangles whose circle holds it and joins the edges
    // of the hole to itself.
    std::vector<std::array<int, 3>> tri = {{np, np + 1, np + 2}};
    for(int i = 0; i < np; i++)
    {
        std::vector<std::array<int, 3>> keep;
        std::map<std::pair<int, int>, int> edges;
        for(const auto& t : tri)
        {
            if(!in_circle(p[t[0]], p[t[1]], p[t[2]], p[i]))
            {
                keep.push_back(t);
                continue;
            }
            for(int j = 0; j < 3; j++) edges[{t[j], t[(j + 1) % 3]}]++;
        }
        for(const auto& kv : edges)
        {
            const int a = kv.first.first, b = kv.first.second;
            if(edges.count({b, a})) continue;
            if(orient(p[a], p[b], p[i]) <= 0)
            {
                snprintf(why, len, "entry %d makes a flat triangle", ent[i]);
                return false;
            }
            keep.push_back({a, b, i});
        }
        tri.swap(keep);
    }
    std::vector<std::array<int, 3>> in;
    int64_t                         area = 0;
    for(const auto& t : tri)
        if(t[0] < np && t[1] < np && t[2] < np)
        {
            in.push_back(t);
            area += orient(p[t[0]], p[t[1]], p[t[2]]);
        }
    p.resize(np);
    if(area != hull_area2(p))
    {
        snprintf(why, len, "the triangles cover %lld of the hull's %lld", static_cast<long long>(area),
                 static_cast<long long>(hull_area2(p)));
        return false;
    }
    if(in.size() >= TONE_TRI_NONE)
    {
        snprintf(why, len, "%zu triangles, more than a byte indexes", in.size());
        return false;
    }

    std::map<std::pair<int, int>, int> owner;
    for(size_t k = 0; k < in.size(); k++)
        for(int j = 0; j < 3; j++) owner[{in[k][j], in[k][(j + 1) % 3]}] = static_cast<int>(k);
    out.clear();
    for(const auto& t : in)
    {
        int ord[3] = {0, 1, 2};
        std::sort(ord, ord + 3, [&](int a, int b) {
            const int ea = ent[t[a]], eb = ent[t[b]];
            return yv[2 * ea] < yv[2 * eb] || (yv[2 * ea] == yv[2 * eb] && ea < eb);
        });
        tone_tri r{};
        for(int j = 0; j < 3; j++)
        {
            const int o = ord[j];
            r.v[j]      = static_cast<uint8_t>(ent[t[o]]);
            auto it     = owner.find({t[(o + 2) % 3], t[(o + 1) % 3]});
            r.nb[j]     = it == owner.end() ? TONE_TRI_NONE : static_cast<uint8_t>(it->second);
        }
        pt q[3];
        for(int j = 0; j < 3; j++) q[j] = {yv[2 * r.v[j]], yv[2 * r.v[j] + 1]};
        r.sign = orient(q[0], q[1], q[2]) > 0 ? 1 : -1;
        out.push_back(r);
    }

    std::vector<int> by(ent.size());
    for(size_t i = 0; i < by.size(); i++) by[i] = static_cast<int>(i);
    std::sort(by.begin(), by.end(), [&](int a, int b) { return p[a].x < p[b].x || (p[a].x == p[b].x && p[a].y < p[b].y); });
    std::vector<int> h;
    for(int i : by)
    {
        while(h.size() >= 2 && orient(p[h[h.size() - 2]], p[h.back()], p[i]) >= 0) h.pop_back();
        h.push_back(i);
    }
    top.clear();
    for(size_t i = 0; i < h.size(); i++)
    {
        if(i > 0 && p[h[i]].x <= p[h[i - 1]].x)
        {
            snprintf(why, len, "the upper hull is not a function of Y at entry %d", ent[h[i]]);
            return false;
        }
        top.push_back(static_cast<uint8_t>(ent[h[i]]));
    }
    if(p[h.front()].y != 0 || p[h.back()].y != 0)
    {
        snprintf(why, len, "the upper hull does not run from a neutral to a neutral");
        return false;
    }
    return true;
}

int tone_photo_locate(const std::vector<tone_tri>& tri, const int16_t* yv, int y, int v)
{
    for(size_t k = 0; k < tri.size(); k++)
    {
        int32_t w[3];
        tone_photo_area(tri[k], yv, y, v, w);
        if(w[0] >= 0 && w[1] >= 0 && w[2] >= 0) return static_cast<int>(k);
    }
    return -1;
}

bool tone_photo_grid(const std::vector<tone_tri>& tri, const int16_t* yv, const std::vector<tone_photo_sample>& s,
                     uint8_t grid[], int* outside)
{
    const int                     NC = TONE_PHOTO_GRID * TONE_PHOTO_GRID;
    std::vector<std::vector<int>> votes(NC, std::vector<int>(tri.size(), 0));
    std::vector<bool>             got(NC, false);
    *outside = 0;
    for(const tone_photo_sample& q : s)
    {
        const int k = tone_photo_locate(tri, yv, q.y, q.v);
        if(k < 0)
        {
            (*outside)++;
            continue;
        }
        votes[q.cell][k]++;
        got[q.cell] = true;
    }
    bool any = false;
    for(int c = 0; c < NC; c++)
        if(got[c])
        {
            any     = true;
            grid[c] = static_cast<uint8_t>(std::max_element(votes[c].begin(), votes[c].end()) - votes[c].begin());
        }
    if(!any) return false;
    for(int c = 0; c < NC; c++)
    {
        if(got[c]) continue;
        int best = -1, bd = 0;
        for(int o = 0; o < NC; o++)
        {
            if(!got[o]) continue;
            const int di = c / TONE_PHOTO_GRID - o / TONE_PHOTO_GRID, dj = c % TONE_PHOTO_GRID - o % TONE_PHOTO_GRID;
            const int d  = di * di + dj * dj;
            if(best < 0 || d < bd) best = o, bd = d;
        }
        grid[c] = grid[best];
    }
    return true;
}
