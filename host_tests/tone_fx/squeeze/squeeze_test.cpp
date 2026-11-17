// Host test of tone_fx_squeeze() (main/tone_fx_squeeze.h, as its stand in in
// ../model/tone_fx_squeeze_model.cpp) against the float squeeze of main/tonemap_squeeze.h followed by
// the floor on L at the black entry, as make_target() in main/tones.cpp runs it.
//
// Sets, each sRGB through the float Oklab conversion below (a copy of the one in main/tones.cpp):
//   gray    all 256 grays. Co, Ao and Bo must be exactly 0 and Lo the input L in q15 held at the
//           black entry's L.
//   edge    the image edge, (v, 0, 0) then (255, v, v), 512 colours
//   random  RANDOM_N colours from xorshift32 with a fixed seed
//   cube    every sRGB colour, 2^24
// Errors are in real units: L the difference of Lo and the reference L, C the difference of Co and
// the reference chroma, ab the distance between (Ao, Bo) and the reference a and b. In every set a
// colour whose float chroma along the hue of sRGB red is 0 or below must get Co, Ao and Bo exactly
// 0. The test fails when a set's largest L or C error is above LIMIT, a gray is off or such a
// colour has chroma.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tonemap_squeeze.h"
#include "tone_fx_squeeze.h"
#include "tone_tables.h"

namespace
{
const float LIMIT    = 0.002f;
const int   RANDOM_N = 200000;

float s_byte_lin[256];

void lin_to_oklab(const float c[3], float out[3])
{
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void oklab(uint8_t r8, uint8_t g8, uint8_t b8, float out[3])
{
    const float c[3] = {s_byte_lin[r8], s_byte_lin[g8], s_byte_lin[b8]};
    lin_to_oklab(c, out);
}

float s_black_l = 0.0f;

// Error histogram in steps of 1e-5, the last bin holding everything from 0.005 up.
struct stat
{
    static const int BINS = 501;
    uint64_t         bin[BINS] = {};
    uint64_t         n         = 0;
    double           sum       = 0.0;
    float            max       = 0.0f;
    uint32_t         worst_rgb = 0;
    float            worst_l = 0.0f, worst_c = 0.0f; // the worst colour's reference L and chroma

    void add(float e, uint32_t rgb, float ref_l, float ref_c)
    {
        bin[std::min(BINS - 1, static_cast<int>(e * 1e5f))]++;
        n++;
        sum += e;
        if(e > max) max = e, worst_rgb = rgb, worst_l = ref_l, worst_c = ref_c;
    }
    float pct(double p) const
    {
        const uint64_t want = static_cast<uint64_t>(p * static_cast<double>(n));
        uint64_t       acc  = 0;
        for(int i = 0; i < BINS; i++)
        {
            acc += bin[i];
            if(acc > want) return static_cast<float>(i + 1) * 1e-5f;
        }
        return max;
    }
    void print(const char* what) const
    {
        printf("  %-2s max %.6f mean %.6f p50 <%.5f p90 <%.5f p99 <%.5f p99.9 <%.5f  worst sRGB %3u %3u %3u ref L "
               "%.4f C %.4f\n",
               what, max, n ? sum / static_cast<double>(n) : 0.0, pct(0.5), pct(0.9), pct(0.99), pct(0.999),
               (worst_rgb >> 16) & 255, (worst_rgb >> 8) & 255, worst_rgb & 255, worst_l, worst_c);
    }
};

struct set_stats
{
    stat     L, C, ab;
    int      gray_bad = 0;
    uint64_t off_hue = 0, off_hue_bad = 0; // colours with float chroma along the hue <= 0, and those with Co != 0
};

// Runs the colours through both forms and adds their errors to st.
void run(const std::vector<uint32_t>& rgb, set_stats* st, bool gray)
{
    const int        n = tone_fx_pad8(static_cast<int>(rgb.size()));
    std::vector<q15> buf(7 * static_cast<size_t>(n) + 8);
    q15*             base = reinterpret_cast<q15*>((reinterpret_cast<uintptr_t>(buf.data()) + 15) & ~uintptr_t(15));
    q15 *            L = base, *A = L + n, *B = A + n, *Lo = B + n, *Ao = Lo + n, *Bo = Ao + n, *Co = Bo + n;
    std::vector<float> ref(3 * static_cast<size_t>(n));
    std::vector<bool>  off(static_cast<size_t>(n));
    for(int i = 0; i < n; i++)
    {
        const uint32_t c = rgb[std::min(i, static_cast<int>(rgb.size()) - 1)];
        float          lab[3];
        oklab(static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c), lab);
        L[i] = q15_from(lab[0]), A[i] = q15_from(lab[1]), B[i] = q15_from(lab[2]);
        off[static_cast<size_t>(i)] = lab[1] * s_squeeze_hue[0] + lab[2] * s_squeeze_hue[1] <= 0.0f;
        tonemap_squeeze(lab);
        lab[0] = std::max(lab[0], s_black_l);
        memcpy(&ref[3 * static_cast<size_t>(i)], lab, sizeof(lab));
    }
    std::vector<q15> lin(L, L + n);
    tone_fx_squeeze(L, A, B, n, Lo, Ao, Bo, Co);
    for(int i = 0; i < static_cast<int>(rgb.size()); i++)
    {
        const float* r     = &ref[3 * static_cast<size_t>(i)];
        const float  ref_c = sqrtf(r[1] * r[1] + r[2] * r[2]);
        st->L.add(fabsf(q15_to(Lo[i]) - r[0]), rgb[i], r[0], ref_c);
        st->C.add(fabsf(q15_to(Co[i]) - ref_c), rgb[i], r[0], ref_c);
        st->ab.add(hypotf(q15_to(Ao[i]) - r[1], q15_to(Bo[i]) - r[2]), rgb[i], r[0], ref_c);
        if(off[static_cast<size_t>(i)])
        {
            st->off_hue++;
            st->off_hue_bad += Co[i] != 0 || Ao[i] != 0 || Bo[i] != 0;
        }
        if(gray && (Co[i] != 0 || Ao[i] != 0 || Bo[i] != 0 || Lo[i] != std::max(lin[i], q15_from(s_black_l))))
        {
            if(st->gray_bad < 5)
                printf("  gray %u: Lin %d Lo %d Ao %d Bo %d Co %d\n", rgb[i] & 255, lin[i], Lo[i], Ao[i], Bo[i], Co[i]);
            st->gray_bad++;
        }
    }
}

bool report(const char* name, const set_stats& st, size_t count)
{
    printf("%s, %zu colours\n", name, count);
    st.L.print("L");
    st.C.print("C");
    st.ab.print("ab");
    printf("  %llu colours with chroma along the hue <= 0, %llu of them with Co, Ao or Bo not 0\n",
           static_cast<unsigned long long>(st.off_hue), static_cast<unsigned long long>(st.off_hue_bad));
    const bool ok = st.L.max <= LIMIT && st.C.max <= LIMIT && st.gray_bad == 0 && st.off_hue_bad == 0;
    if(st.gray_bad) printf("  %d grays off\n", st.gray_bad);
    if(!ok) printf("  FAIL: limit %.4f\n", LIMIT);
    return ok;
}

// Same in place as out of place, on the random set.
int in_place_diff(const std::vector<uint32_t>& rgb)
{
    const int        n = tone_fx_pad8(static_cast<int>(rgb.size()));
    std::vector<q15> buf(10 * static_cast<size_t>(n) + 8);
    q15*             b = reinterpret_cast<q15*>((reinterpret_cast<uintptr_t>(buf.data()) + 15) & ~uintptr_t(15));
    for(int i = 0; i < n; i++)
    {
        const uint32_t c = rgb[std::min(i, static_cast<int>(rgb.size()) - 1)];
        float          lab[3];
        oklab(static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c), lab);
        b[i] = q15_from(lab[0]), b[n + i] = q15_from(lab[1]), b[2 * n + i] = q15_from(lab[2]);
    }
    q15* o = b + 3 * n;
    tone_fx_squeeze(b, b + n, b + 2 * n, n, o, o + n, o + 2 * n, o + 3 * n);
    q15* co = o + 4 * n;
    tone_fx_squeeze(b, b + n, b + 2 * n, n, b, b + n, b + 2 * n, co);
    int d = 0;
    for(int i = 0; i < 3 * n; i++) d += b[i] != o[i];
    for(int i = 0; i < n; i++) d += co[i] != o[3 * n + i];
    return d;
}
} // namespace

int main()
{
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        s_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    tonemap_squeeze_begin(oklab);
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        if(strcmp(k_tonemap[e].pattern, TONEMAP_BLACK_PATTERN) == 0) s_black_l = k_tonemap[e].L;
    printf("edge fits against the float tables, largest miss over every q15 L: image %.6f panel %.6f\n",
           static_cast<double>(tone_tables::squeeze_fit_img), static_cast<double>(tone_tables::squeeze_fit_pan));

    bool ok = true;
    {
        std::vector<uint32_t> rgb;
        for(uint32_t v = 0; v < 256; v++) rgb.push_back(v << 16 | v << 8 | v);
        set_stats st;
        run(rgb, &st, true);
        ok &= report("gray", st, rgb.size());
    }
    {
        std::vector<uint32_t> rgb;
        for(uint32_t v = 0; v < 256; v++) rgb.push_back(v << 16);
        for(uint32_t v = 0; v < 256; v++) rgb.push_back(255u << 16 | v << 8 | v);
        set_stats st;
        run(rgb, &st, false);
        ok &= report("edge black to red to white", st, rgb.size());
    }
    std::vector<uint32_t> rnd;
    {
        uint32_t s = 0x2545f491u;
        for(int i = 0; i < RANDOM_N; i++)
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            rnd.push_back(s & 0xffffffu);
        }
        set_stats st;
        run(rnd, &st, false);
        ok &= report("random", st, rnd.size());
    }
    {
        set_stats             st;
        std::vector<uint32_t> rgb(65536);
        for(uint32_t r = 0; r < 256; r++)
        {
            for(uint32_t i = 0; i < 65536; i++) rgb[i] = r << 16 | i;
            run(rgb, &st, false);
        }
        ok &= report("cube", st, size_t(1) << 24);
    }
    const int d = in_place_diff(rnd);
    printf("in place against out of place, random set: %d elements differ\n", d);
    ok &= d == 0;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
