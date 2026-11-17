// Check of the pass A tables of main/tone_tables.inc against pass_a() of main/tones.cpp itself.
//
// tone_tables_gen.cpp builds the slot WHITE and the solid gray table through its own copy of the
// steps of pass_a(). This program includes main/tones.cpp, so it runs the dither's own pass_a() on
// the stand ins of the kernels with the tables of main/tone_tables.inc, as the host harness does,
// and compares. A change to pass_a() or its constants that the generator does not follow fails
// here. Exit status 0 when both tables match.

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
    tone_solid_gray_table(gray, s_qL.data(), solid);
    int differ = 0;
    for(int g = 0; g < 256; g++)
        if(solid[g] != tones_solid_gray(g))
        {
            if(differ < 8)
                printf("FAIL gray %d: pass_a gives %u, the file holds %d\n", g, solid[g], tones_solid_gray(g));
            differ++;
        }
    bad += differ;

    printf("tone_tables_check: slot WHITE %s, solid gray %d of 256 differ: %s\n",
           same(w, t) ? "same" : "DIFFERS", differ, bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
