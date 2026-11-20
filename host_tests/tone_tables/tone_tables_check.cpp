// Check of the pass A tables of main/tone_tables.inc against pass_a() of main/tones.cpp itself.
//
// tone_tables_gen.cpp builds the slot WHITE and the solid gray table through its own copy of the
// steps of pass_a(). This program includes main/tones.cpp, so it runs the dither's own pass_a() on
// the stand ins of the kernels with the tables of main/tone_tables.inc, as the host harness does,
// and compares. A change to pass_a() or its constants that the generator does not follow fails
// here. It also runs the photo dither of tones.cpp on the photo tables and checks that its picks
// average to their targets. It checks the phase 2 marks h and '|' in the expansion and in the plans
// of tone_plan_code(). Exit status 0 when both tables match, every photo mix holds and every mark
// check passes.

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

static bool same(const tone_white_slot& a, const tone_white_slot& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.L == b.L && a.A == b.A && a.B == b.B && a.C == b.C &&
           a.lo == b.lo && a.hi == b.hi && a.kind == b.kind && a.ent == b.ent && a.white_kind == b.white_kind;
}

int main()
{
    int bad = check_marks();

    // The slot WHITE, as tones_init() built it from the kernels: 8 white pixels, slot 0 in use, the
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

    // Every gray byte in chunks, as tones_solid_gray() ran pass_a(), then the choice of
    // tone_tables_build.h.
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
                const unsigned e = photo_entry(s, r);
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
    return bad ? 1 : 0;
}
