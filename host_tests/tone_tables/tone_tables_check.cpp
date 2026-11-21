// Check of the pass A tables of main/tone_tables.inc against pass_a() of main/tones.cpp itself.
//
// tone_tables_gen.cpp builds the slot WHITE and the solid gray table through its own copy of the
// steps of pass_a(). This program includes main/tones.cpp, so it runs the dither's own pass_a() on
// the stand ins of the kernels with the tables of main/tone_tables.inc, as the host harness does,
// and compares. A change to pass_a() or its constants that the generator does not follow fails
// here. It also runs the photo dither of tones.cpp on the photo tables and checks that its picks
// average to their targets, and the nearest mix on the graph of the tones (check_near()). It checks
// the phase 2 marks h and '|' in the expansion and in the plans of tone_plan_code(). Exit status 0
// when both tables match, every photo mix holds and every mark check passes.

#include "tones.cpp"

#include <cstdio>
#include <string>

#include "tone_plan_expand.h"
#include "tone_tables_build.h"

// The phase 2 codes of entry e in plan, as letters of hpiw.
static std::string p2_codes(const tone_plan& plan, unsigned e)
{
    std::string out;
    for(unsigned k = 0; k < plan.len[TONE_PHASE_2]; k++) out += "hpiw"[tone_plan_code(plan, e, TONE_PHASE_2, k)];
    return out;
}

// The phase 2 that the notation gives s in len pulls. The nudge and p2 are right aligned. With a
// '|', the nudge and the letters before it start at pull 0 instead, and the holds sit between.
static std::string p2_want(const tone_seq& s, unsigned len)
{
    const size_t nd = static_cast<size_t>(s.nudge < 0 ? -s.nudge : s.nudge);
    std::string  own(nd, s.nudge > 0 ? 'i' : 'p');
    own += s.p2;
    const size_t head = s.split < 0 ? 0 : nd + static_cast<size_t>(s.split);
    return own.substr(0, head) + std::string(len - own.size(), 'h') + own.substr(head);
}

// The marks h and '|'. Expansions of fixed patterns and patterns the notation refuses. For every
// entry, the pattern without its marks has the same net, balance and push, when the marks move no
// phase 1 letter. For the plan of every entry and for the plan of the neutral entries, each entry's
// phase 2 is that of p2_want() and the plan is as long as the longest nudge plus n2.
static int check_marks()
{
    int  bad = 0;
    char why[192];
    struct expand_case
    {
        const char* src;
        const char* p2;
        int         split;
    };
    static const expand_case k_expand[] = {
        {"M4i2p5h4i", "iippppphhhhi", -1}, {"M3i2p8hi", "iipppppppphi", -1},
        {"M4i2p5ip|i", "iipppppipi", 9},   {"M3i2p5i5|ph", "iipppppiiiiiph", 12},
        {"M3i2|", "ii", 2},                {"M3|I", "ii", 0},
        {"M3|", "", 0},                    {"M4i2p5", "iippppp", -1},
    };
    for(const expand_case& c : k_expand)
    {
        tone_seq s;
        if(!tone_pattern_expand(c.src, &s, why, sizeof(why)))
        {
            bad++;
            printf("FAIL expand %s: %s\n", c.src, why);
        }
        else if(strcmp(s.p2, c.p2) != 0 || s.split != c.split || s.n2 != strlen(c.p2))
        {
            bad++;
            printf("FAIL expand %s: p2 %s split %d, want %s split %d\n", c.src, s.p2, s.split, c.p2, c.split);
        }
    }
    static const char* const k_refuse[] = {"M3i|p|i", "M3i|2p", "M3|S", "M3i|M", "M3hM", "M3x"};
    for(const char* src : k_refuse)
    {
        tone_seq s;
        if(tone_pattern_expand(src, &s, why, sizeof(why)))
        {
            bad++;
            printf("FAIL expand %s: the notation refuses it, the expansion did not\n", src);
        }
    }

    unsigned bare_n = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
    {
        const char* src = k_tonemap[e].pattern;
        std::string bare;
        for(size_t k = 0; src[k] != '\0'; k++)
        {
            if(src[k] == 'h')
                while(isdigit(static_cast<unsigned char>(src[k + 1]))) k++;
            else if(src[k] != '|') bare += src[k];
        }
        if(bare == src) continue;
        tone_seq  a, b;
        tone_push ua, ub;
        if(!tone_pattern_expand(src, &a, why, sizeof(why)) || !tone_pattern_expand(bare.c_str(), &b, why, sizeof(why)))
        {
            bad++;
            printf("FAIL entry %u %s or %s: %s\n", e, src, bare.c_str(), why);
            continue;
        }
        if(strcmp(a.p1a, b.p1a) != 0 || strcmp(a.p1b, b.p1b) != 0) continue;
        tone_push_of(a, &ua);
        tone_push_of(b, &ub);
        if(a.net != b.net || a.prefix != b.prefix || a.nudge != b.nudge || a.residual != b.residual ||
           memcmp(&ua, &ub, sizeof(ua)) != 0)
        {
            bad++;
            printf("FAIL entry %u %s: its net, balance or push differs from that of %s\n", e, src, bare.c_str());
        }
        bare_n++;
    }

    if(!tone_plan_init(why, sizeof(why)))
    {
        printf("FAIL tone_plan_init: %s\n", why);
        return bad + 1;
    }
    uint32_t all[TONEMAP_COUNT], neutral[TONEMAP_COUNT] = {};
    for(uint32_t& c : all) c = 1;
    for(int i = 0; i < tone_tables::neutral_count; i++) neutral[tone_tables::neutral[i]] = 1;
    int plan_bad = 0;
    for(const uint32_t* count : {all, neutral})
    {
        tone_plan plan;
        tone_plan_build(count, &plan);
        const unsigned len  = plan.len[TONE_PHASE_2];
        unsigned       most = 0;
        for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        {
            if(!count[e]) continue;
            const tone_seq& s   = tone_tables::seq[e];
            const unsigned  own = static_cast<unsigned>(s.nudge < 0 ? -s.nudge : s.nudge) + s.n2;
            most                = std::max(most, own);
            if(own > len) continue;
            const std::string got = p2_codes(plan, e), want = p2_want(s, len);
            if(got != want)
            {
                plan_bad++;
                printf("FAIL plan entry %u %s: phase 2 %s, want %s\n", e, k_tonemap[e].pattern, got.c_str(),
                       want.c_str());
            }
        }
        if(most != len)
        {
            plan_bad++;
            printf("FAIL plan: phase 2 of %u pulls, the longest entry needs %u\n", len, most);
        }
    }
    bad += plan_bad;
    printf("tone_plan marks: %zu expansions, %zu refusals, %u marked entries match their bare patterns, plans of "
           "every and of the neutral entries %d faults: %s\n",
           sizeof(k_expand) / sizeof(k_expand[0]), sizeof(k_refuse) / sizeof(k_refuse[0]), bare_n, plan_bad,
           bad ? "FAIL" : "PASS");
    return bad;
}

// The point of segment a b, or of triangle a b c for n 3, nearest (tl, tc), in double: the target
// itself inside the triangle, else the nearest point of its edges.
static void reach_of(const double* pl, const double* pc, int n, double tl, double tc, double* rl, double* rc)
{
    auto seg = [&](int i, int j, double* x, double* y) {
        const double bl = pl[j] - pl[i], bc = pc[j] - pc[i];
        double       t  = ((tl - pl[i]) * bl + (tc - pc[i]) * bc) / (bl * bl + bc * bc);
        t               = t < 0 ? 0 : t > 1 ? 1 : t;
        *x = pl[i] + t * bl, *y = pc[i] + t * bc;
    };
    if(n == 1)
    {
        *rl = pl[0], *rc = pc[0];
        return;
    }
    if(n == 2)
    {
        seg(0, 1, rl, rc);
        return;
    }
    int pos = 0, neg = 0;
    for(int i = 0; i < 3; i++)
    {
        const int    j = (i + 1) % 3;
        const double s = (pl[j] - pl[i]) * (tc - pc[i]) - (pc[j] - pc[i]) * (tl - pl[i]);
        pos += s >= 0, neg += s <= 0;
    }
    if(pos == 3 || neg == 3)
    {
        *rl = tl, *rc = tc;
        return;
    }
    double bd = 1e30;
    for(int i = 0; i < 3; i++)
    {
        double x, y;
        seg(i, (i + 1) % 3, &x, &y);
        const double d = std::hypot(x - tl, y - tc);
        if(d < bd) bd = d, *rl = x, *rc = y;
    }
}

// The nearest mix (TONE_PHOTO_NEAREST) on the graph of the file, for sRGB colours on a step of 5
// through pass_a(). Checks:
// 1. Every entry a coloured pixel takes at any rank is an entry of its mix with weight, and the
//    entries of a mix with weight are neighbours of each other.
// 2. The mean of the picks of photo_entry<true>() over the 256 ranks lies within 1/256 of the spread
//    of the entries and 1 count of the point the mix reaches, computed here in double: in a triangle
//    the point of it nearest the target, outside the triangles the point of the mix's weights.
// 3. A neutral target takes no entry with r or R at any rank.
// 4. A target on a tone that owns its own point takes that tone at every rank.
// It runs with the linear weights (both steps of the ease off) and restores the ease after. Prints how the mixes end,
// the spread of the mixes and the largest colour error. Returns the faults.
static int check_near()
{
    int ease[4];
    tones_near_ease(&ease[0], &ease[1], &ease[2], &ease[3]);
    tones_set_near_ease(0, 0, 0, 0);
    if(!tone_graph_hydrate(&s_graph, tone_tables::qL.data(), tone_tables::graph_c.data(), tone_tables::neutral.data(),
                           tone_tables::neutral_count, tone_tables::graph_ring.data(), tone_tables::graph_ring_count,
                           tone_tables::graph_top.data(), tone_tables::graph_top_count, s_graph_cell.data(),
                           s_graph_cand.data(), tone_tables::graph_rows))
    {
        printf("FAIL nearest mix: the graph does not hydrate\n");
        return 1;
    }
    const tone_graph& g = s_graph;
    chunk&            c = s_ck;
    auto touch = [&](unsigned a, unsigned b) {
        if(a == b) return true;
        for(unsigned k = g.at[a]; k < g.at[a + 1]; k++)
            if(g.nb[k] == b) return true;
        return false;
    };
    auto tinted = [](unsigned e) { return strpbrk(k_tonemap[e].pattern, "rR") != nullptr; };

    int                 far = 0, mean_bad = 0, tint = 0, on_bad = 0, paper_red = 0, n = 0;
    int                 how[2] = {};
    std::vector<double> spread;
    double              worst = 0, err_max = 0, sp_max = 0;
    int                 err_px[3] = {}, sp_v[3] = {}, sp_n = 0;
    double              err_t[2] = {};
    uint8_t             px[3 * CHUNK + 16];
    const int           S = 52;
    for(int base = 0; base < S * S * S; base += CHUNK)
    {
        const int cnt = std::min(CHUNK, S * S * S - base);
        memset(px, 0, sizeof(px));
        for(int i = 0; i < cnt; i++)
        {
            const int v   = base + i;
            px[3 * i]     = static_cast<uint8_t>(5 * (v / (S * S)));
            px[3 * i + 1] = static_cast<uint8_t>(5 * (v / S % S));
            px[3 * i + 2] = static_cast<uint8_t>(5 * (v % S));
        }
        tone_fx_srgb_to_lin(px, tone_fx_pad8(cnt), c.r, c.g, c.b);
        pass_a(cnt);
        for(int s = 0; s < cnt; s++)
        {
            if(c.kind[s] != K_COLOUR)
            {
                for(int r = 0; r < 256; r++) tint += tinted(photo_entry<true>(s, r));
                continue;
            }
            const int     tl = tone_graph_l12(c.L[s]);
            const int     tc = tone_graph_c12(c.C[s]);
            tone_near_mix m;
            tone_near_pick(near_graph{g}, s_near_ease, tl, tc, &m);
            how[m.how]++;
            n++;
            double ml = 0, mc = 0;
            bool   near = true;
            for(int r = 0; r < 256; r++)
            {
                const unsigned e = photo_entry<true>(s, r);
                bool           in = false;
                for(int i = 0; i < 3; i++) in = in || (m.v[i] == e && m.w[i] > 0);
                near = near && in;
                ml += g.l[e] / 256.0, mc += g.c[e] / 256.0;
            }
            for(int i = 0; i < 3; i++)
                for(int j = i + 1; j < 3; j++) near = near && (m.w[i] == 0 || m.w[j] == 0 || touch(m.v[i], m.v[j]));
            far += !near;
            // The corners of the mix: three for a triangle, else the entries with weight.
            const bool tri = m.how == TONE_NEAR_IN && m.v[0] != m.v[1] && m.v[1] != m.v[2] && m.v[0] != m.v[2];
            uint8_t    mv[3];
            int        mn = 0;
            for(int i = 0; i < 3; i++)
                if(tri || m.w[i] > 0) mv[mn++] = m.v[i];
            double pl[3], pc[3], sp = 0;
            for(int i = 0; i < mn; i++) pl[i] = g.l[mv[i]], pc[i] = g.c[mv[i]];
            for(int i = 0; i < mn; i++)
                for(int j = i + 1; j < mn; j++) sp = std::max(sp, std::hypot(pl[i] - pl[j], pc[i] - pc[j]));
            // In a triangle the point of it nearest the target, outside the triangles the point the
            // weights of the mix give, the point of the focal rule that tone_graph_check checks.
            double rl, rc;
            if(m.how == TONE_NEAR_IN) reach_of(pl, pc, mn, tl, tc, &rl, &rc);
            else
            {
                const double s = static_cast<double>(m.w[0]) + m.w[1] + m.w[2];
                rl = rc = 0;
                for(int i = 0; i < 3; i++) rl += m.w[i] / s * g.l[m.v[i]], rc += m.w[i] / s * g.c[m.v[i]];
            }
            const double miss = std::max(fabs(ml - rl), fabs(mc - rc)) / (sp / 256 + 1);
            worst             = std::max(worst, miss);
            if(miss > 1 && mean_bad++ < 8)
                printf("FAIL nearest mix %u %u %u: reaches L %.1f 2C %.1f, mean of the picks %.1f %.1f\n", px[3 * s],
                       px[3 * s + 1], px[3 * s + 2], rl, rc, ml, mc);
            if(mn > 1)
            {
                spread.push_back(sp);
                if(sp > sp_max)
                {
                    sp_max = sp, sp_n = mn;
                    for(int i = 0; i < mn; i++) sp_v[i] = mv[i];
                }
            }
            const double err = std::hypot(rl - tl, rc - tc);
            if(err > err_max)
            {
                err_max = err, err_t[0] = tl, err_t[1] = tc;
                for(int j = 0; j < 3; j++) err_px[j] = px[3 * s + j];
            }
            bool paper = false, red = false;
            for(int i = 0; i < 3; i++)
                if(m.w[i] > 0) paper = paper || m.v[i] == TONE_PAPER, red = red || g.c[m.v[i]] >= 0.05 * 8192;
            paper_red += paper && red;
        }
    }
    int own = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
    {
        if(g.c[e] < TONE_GRAPH_STRIP || tone_graph_owner(g, g.l[e], g.c[e]) != e) continue;
        tone_near_mix m;
        tone_near_pick(near_graph{g}, s_near_ease, g.l[e], g.c[e], &m);
        for(int r = 0; r < 256; r++) on_bad += tone_near_choose(m, r) != e;
        own++;
    }
    tones_set_near_ease(ease[0], ease[1], ease[2], ease[3]);
    std::sort(spread.begin(), spread.end());
    const double q    = 1.0 / TONE_GRAPH_ONE;
    const int    bad  = far + mean_bad + tint + on_bad;
    auto         pct  = [&](int k) { return 100.0 * k / n; };
    printf("nearest mix: %d coloured targets: %.1f%% in a triangle, %.1f%% outside the triangles on the nearest point "
           "of the rim\n",
           n, pct(how[TONE_NEAR_IN]), pct(how[TONE_NEAR_RIM]));
    if(!spread.empty())
        printf("nearest mix: spread of a mix in the plane, median %.4f, 95th percentile %.4f, largest %.4f: %d tones",
               spread[spread.size() / 2] * q, spread[spread.size() * 95 / 100] * q, sp_max * q, sp_n);
    for(int i = 0; i < sp_n; i++)
        printf(" %d %s (L %.3f C %.3f)", sp_v[i], k_tonemap[sp_v[i]].pattern, g.l[sp_v[i]] * q, g.c[sp_v[i]] * q / 2);
    printf("\nnearest mix: largest distance from the target to the point reached %.4f, sRGB "
           "%d %d %d, target L %.4f C %.4f\n",
           err_max * q, err_px[0], err_px[1], err_px[2], err_t[0] * q, err_t[1] * q / 2);
    printf("nearest mix: mixes with paper and a tone of chroma 0.05 or more %d. Checks: picks off the mix or mixes of "
           "tones that are not neighbours %d, means off the point reached %d (worst %.2f of the bound), neutral targets on r or R %d, "
           "tones on their own point off at a rank %d of %d tones: %s\n",
           paper_red, far, mean_bad, worst, tint, on_bad, own, bad ? "FAIL" : "PASS");
    return bad;
}

static bool same(const tone_white_slot& a, const tone_white_slot& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.L == b.L && a.A == b.A && a.B == b.B && a.C == b.C &&
           a.lo == b.lo && a.hi == b.hi && a.kind == b.kind && a.ent == b.ent && a.white_kind == b.white_kind;
}

int main()
{
    int bad = check_marks();

    // The slot WHITE from the kernels: 8 white pixels, slot 0 in use, the
    // chunk's arrays as they are before the first chunk.
    chunk&  c = s_ck;
    uint8_t white[3 * 8 + 16];
    memset(white, 255, sizeof(white));
    tone_fx_srgb_to_lin(white, 8, c.r, c.g, c.b);
    pass_a(1);
    tone_white_slot w = {c.r[0], c.g[0], c.b[0], c.L[0], c.A[0],    c.B[0], c.C[0],
                         c.lo[0], c.hi[0], c.kind[0], NO_ENTRY, K_ENTRY};
    if(c.kind[0] != K_ENTRY || c.lo[0] != TONE_PAPER)
    {
        w.white_kind = c.kind[0];
        w.kind       = K_WHITE;
    }
    w.ent                    = w.kind == K_ENTRY ? w.lo : NO_ENTRY;
    const tone_white_slot& t = tone_tables::white;
    if(!same(w, t))
    {
        bad++;
        printf("FAIL slot WHITE: pass_a gives r %d g %d b %d L %d A %d B %d C %d lo %u hi %u kind %u ent %u "
               "white_kind %u, the file holds r %d g %d b %d L %d A %d B %d C %d lo %u hi %u kind %u ent %u "
               "white_kind %u\n",
               w.r, w.g, w.b, w.L, w.A, w.B, w.C, w.lo, w.hi, w.kind, w.ent, w.white_kind, t.r, t.g, t.b, t.L, t.A,
               t.B, t.C, t.lo, t.hi, t.kind, t.ent, t.white_kind);
    }

    // Every gray byte in chunks through pass_a(), then the choice of tone_tables_build.h.
    tone_gray_target gray[256];
    uint8_t          px[3 * CHUNK + 16];
    for(int v0 = 0; v0 < 256; v0 += CHUNK)
    {
        const int n = std::min(CHUNK, 256 - v0);
        memset(px, 0, sizeof(px));
        for(int i = 0; i < 3 * n; i++) px[i] = static_cast<uint8_t>(v0 + i / 3);
        tone_fx_srgb_to_lin(px, n, c.r, c.g, c.b);
        pass_a(n);
        for(int i = 0; i < n; i++) gray[v0 + i] = {c.L[i], c.kind[i] == K_ENTRY ? c.lo[i] : -1};
    }
    uint8_t solid[256];
    tone_solid_gray_table(gray, tone_tables::qL.data(), solid);
    int differ = 0;
    for(int g = 0; g < 256; g++)
        if(solid[g] != tones_solid_gray(g))
        {
            if(differ < 8)
                printf("FAIL gray %d: pass_a gives %u, the file holds %d\n", g, solid[g], tones_solid_gray(g));
            differ++;
        }
    bad += differ;

    // The photo dither over the 256 threshold ranks, each 16 times in the noise table, for sRGB
    // colours on a step of 15: the mean Y and V of the entries picked against the target's, the
    // target's V held to the hull as photo_entry() holds it. Each of the two cuts between three
    // entries is placed to 1/512, so the miss stays within 1/256 of the spread of the entries mixed,
    // and 1 count of rounding.
    int    photo_n = 0, photo_bad = 0;
    double photo_worst = 0;
    for(int base = 0; base < 18 * 18 * 18; base += CHUNK)
    {
        const int n = std::min(CHUNK, 18 * 18 * 18 - base);
        memset(px, 0, sizeof(px));
        for(int i = 0; i < n; i++)
        {
            const int v = base + i;
            px[3 * i]   = static_cast<uint8_t>(15 * (v / 324));
            px[3 * i + 1] = static_cast<uint8_t>(15 * (v / 18 % 18));
            px[3 * i + 2] = static_cast<uint8_t>(15 * (v % 18));
        }
        tone_fx_srgb_to_lin(px, tone_fx_pad8(n), c.r, c.g, c.b);
        pass_a(n);
        for(int s = 0; s < n; s++)
        {
            int      ty, tv;
            unsigned mix[3] = {c.lo[s], c.hi[s], c.hi[s]};
            if(c.kind[s] == K_COLOUR)
            {
                tone_photo_yv(c.r[s], c.g[s], c.b[s], &ty, &tv);
                tv = tone_photo_clamp(s_photo_top.data(), tone_tables::photo_top_count, s_photo_yv.data(), ty, tv);
                int32_t        wt[3];
                const unsigned k = tone_photo_find(s_photo_tri.data(), s_photo_yv.data(),
                                                   s_photo_grid[static_cast<size_t>(tone_photo_cell(c.L[s], c.C[s]))],
                                                   ty, tv, wt);
                for(int j = 0; j < 3; j++) mix[j] = s_photo_tri[k].v[j];
            }
            else ty = c.kind[s] == K_NEUTRAL ? c.r[s] : s_photo_yv[2 * c.lo[s]], tv = 0;
            int lo_y = 32767, hi_y = 0, lo_v = 32767, hi_v = -32768;
            for(unsigned e : mix)
            {
                const int y = s_photo_yv[2 * e], v = s_photo_yv[2 * e + 1];
                lo_y = std::min(lo_y, y), hi_y = std::max(hi_y, y), lo_v = std::min(lo_v, v), hi_v = std::max(hi_v, v);
            }
            double my = 0, mv = 0;
            for(int r = 0; r < 256; r++)
            {
                const unsigned e = photo_entry<false>(s, r);
                my += s_photo_yv[2 * e] / 256.0, mv += s_photo_yv[2 * e + 1] / 256.0;
            }
            const double by = (hi_y - lo_y) / 256.0 + 1, bv = (hi_v - lo_v) / 256.0 + 1;
            const double ey = fabs(my - ty) / by, ev = fabs(mv - tv) / bv;
            photo_worst     = std::max(photo_worst, std::max(ey, ev));
            if(ey > 1 || ev > 1)
            {
                if(photo_bad < 8)
                    printf("FAIL photo %u %u %u: target Y %d V %d, mean of the picks Y %.1f V %.1f\n", px[3 * s],
                           px[3 * s + 1], px[3 * s + 2], ty, tv, my, mv);
                photo_bad++;
            }
            photo_n++;
        }
    }
    bad += photo_bad;

    printf("tone_tables_check: slot WHITE %s, solid gray %d of 256 differ, photo mix %d of %d colours off their "
           "target (worst miss %.2f of the bound): %s\n",
           same(w, t) ? "same" : "DIFFERS", differ, photo_bad, photo_n, photo_worst, bad ? "FAIL" : "PASS");
    bad += check_near();
    return bad ? 1 : 0;
}
