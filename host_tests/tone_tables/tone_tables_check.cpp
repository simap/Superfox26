// Check of the pass A tables of main/tone_tables.inc against pass_a() of main/tones.cpp itself.
//
// tone_tables_gen.cpp builds the slot WHITE and the solid gray table through its own copy of the
// steps of pass_a(). This program includes main/tones.cpp, so it runs the dither's own pass_a() on
// the stand ins of the kernels with the tables of main/tone_tables.inc, as the host harness does,
// and compares. A change to pass_a() or its constants that the generator does not follow fails
// here. It also runs the photo dither of tones.cpp on the photo tables and checks that its picks
// average to their targets. Exit status 0 when both tables match and every photo mix holds.

#include "tones.cpp"

#include <cstdio>

#include "tone_tables_build.h"

static bool same(const tone_white_slot& a, const tone_white_slot& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.L == b.L && a.A == b.A && a.B == b.B && a.C == b.C &&
           a.lo == b.lo && a.hi == b.hi && a.kind == b.kind && a.ent == b.ent && a.white_kind == b.white_kind;
}

int main()
{
    int bad = 0;

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
