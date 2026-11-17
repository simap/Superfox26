// Host test of the Oklab kernels (main/tone_fx_oklab.h, as their stand ins in
// ../model/tone_fx_oklab_model.cpp) against the float code of main/tones.cpp
// (lin_to_oklab, oklab_to_lin, copied below) and against cbrt in double. Prints the error of each
// kernel as a maximum, a mean and a distribution, with the worst input, and exits 1 when a target is
// missed:
//   cube root      at most 3 counts of q15 over all inputs
//   lin to Oklab   at most 4 counts in each of L, a, b over in-gamut colours
//   Oklab to lin   at most 8 counts in each of r, g, b over in-gamut colours
// Errors are against the float result before rounding, in counts of q15.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "tone_fx_oklab.h"

namespace
{
// tones.cpp, verbatim.
inline void lin_to_oklab(const float c[3], float out[3])
{
    const float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0]        = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1]        = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2]        = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void oklab_to_lin(const float lab[3], float out[3])
{
    float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    l       = l * l * l;
    m       = m * m * m;
    s       = s * s * s;
    out[0]  = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1]  = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2]  = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

uint32_t s_seed = 0x2545F491u;
uint32_t rnd()
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

// Error statistics of one output channel. Bins of one count, the last open.
struct stat
{
    const char* name;
    double      max = 0, sum = 0;
    long        n = 0, bins[9] = {};
    int         worst[3] = {};
    double      worst_got = 0, worst_ref = 0;

    void add(double got, double ref, const int in[3])
    {
        const double e = fabs(got - ref);
        sum += e;
        n++;
        bins[std::min(8, static_cast<int>(e))]++;
        if(e > max) max = e, worst[0] = in[0], worst[1] = in[1], worst[2] = in[2], worst_got = got, worst_ref = ref;
    }
    void print() const
    {
        printf("  %-3s max %6.3f mean %.4f  worst at %6d %6d %6d (got %.0f, float %.3f)\n", name, max, sum / n, worst[0],
               worst[1], worst[2], worst_got, worst_ref);
        printf("      share by error [0,1) [1,2) ... [8,):");
        for(int k = 0; k < 9; k++) printf(" %.5f", static_cast<double>(bins[k]) / n);
        printf("\n");
    }
};

float s_byte_lin[256];

bool test_cbrt()
{
    std::vector<q15> x(65536), y(65536);
    for(int i = 0; i < 65536; i++) x[i] = static_cast<q15>(i - 32768);
    tone_fx_cbrt(x.data(), 65536, y.data());
    stat st{"y"};
    int  bad_neg = 0;
    for(int i = 0; i < 65536; i++)
    {
        const int in[3] = {x[i], 0, 0};
        if(x[i] <= 0)
            bad_neg += y[i] != 0;
        else
            st.add(y[i], cbrt(x[i] / 32768.0) * 32768.0, in);
    }
    printf("cube root, all 32767 positive inputs (inputs 0 and below give %s)\n", bad_neg ? "NOT 0" : "0");
    st.print();
    return st.max <= 3.0 && bad_neg == 0;
}

// Runs n pixels through tone_fx_lin_to_oklab and the float code and adds the errors.
void run_lin_to_oklab(const std::vector<q15>& r, const std::vector<q15>& g, const std::vector<q15>& b, stat* st,
                      long* differ_1)
{
    const int        n = static_cast<int>(r.size());
    std::vector<q15> L(n), A(n), B(n);
    tone_fx_lin_to_oklab(r.data(), g.data(), b.data(), n, L.data(), A.data(), B.data());
    for(int i = 0; i < n; i++)
    {
        const float c[3] = {q15_to(r[i]), q15_to(g[i]), q15_to(b[i])};
        float       o[3];
        lin_to_oklab(c, o);
        const int in[3] = {r[i], g[i], b[i]};
        st[0].add(L[i], o[0] * 32768.0, in);
        st[1].add(A[i], o[1] * 32768.0, in);
        st[2].add(B[i], o[2] * 32768.0, in);
        if(differ_1 && i % 61 == 0)
        {
            TONE_FX_ALIGNED q15 px[8] = {r[i], g[i], b[i]};
            TONE_FX_ALIGNED q15 lab[8];
            tone_fx_lin_to_oklab_1(px, lab);
            *differ_1 += lab[0] != L[i] || lab[1] != A[i] || lab[2] != B[i];
        }
    }
}

bool test_lin_to_oklab()
{
    bool ok = true;
    long differ_1 = 0;
    struct set
    {
        const char* what;
        int         kind;
    };
    const set sets[] = {{"every sRGB byte colour through the byte to linear table of tones.cpp", 0},
                        {"4M random q15 colours", 1},
                        {"every colour with r, g, b in 0 to 63 counts (near black)", 2}};
    for(const set& s : sets)
    {
        stat             st[3] = {{"L"}, {"a"}, {"b"}};
        std::vector<q15> r, g, b;
        auto             flush = [&] {
            run_lin_to_oklab(r, g, b, st, &differ_1);
            r.clear(), g.clear(), b.clear();
        };
        auto push = [&](int x, int y, int z) {
            r.push_back(static_cast<q15>(x)), g.push_back(static_cast<q15>(y)), b.push_back(static_cast<q15>(z));
            if(r.size() == 1 << 16) flush();
        };
        if(s.kind == 0)
            for(int x = 0; x < 256; x++)
                for(int y = 0; y < 256; y++)
                    for(int z = 0; z < 256; z++)
                        push(q15_from(s_byte_lin[x]), q15_from(s_byte_lin[y]), q15_from(s_byte_lin[z]));
        else if(s.kind == 1)
            for(int k = 0; k < 4 << 20; k++) push(rnd() & 32767, rnd() & 32767, rnd() & 32767);
        else
            for(int x = 0; x < 64; x++)
                for(int y = 0; y < 64; y++)
                    for(int z = 0; z < 64; z++) push(x, y, z);
        flush();
        printf("lin to Oklab, %s\n", s.what);
        for(const stat& c : st)
        {
            c.print();
            ok = ok && c.max <= 4.0;
        }
    }
    printf("lin to Oklab one pixel form against the array form: %ld of the sampled pixels differ\n", differ_1);
    return ok && differ_1 == 0;
}

void run_oklab_to_lin(const std::vector<q15>& L, const std::vector<q15>& A, const std::vector<q15>& B, stat* st)
{
    const int        n = static_cast<int>(L.size());
    std::vector<q15> r(n), g(n), b(n);
    tone_fx_oklab_to_lin(L.data(), A.data(), B.data(), n, r.data(), g.data(), b.data());
    for(int i = 0; i < n; i++)
    {
        const float lab[3] = {q15_to(L[i]), q15_to(A[i]), q15_to(B[i])};
        float       o[3];
        oklab_to_lin(lab, o);
        const int in[3] = {L[i], A[i], B[i]};
        const q15 got[3] = {r[i], g[i], b[i]};
        for(int k = 0; k < 3; k++)
            st[k].add(got[k], std::min(32767.0, std::max(0.0, static_cast<double>(o[k]) * 32768.0)), in);
    }
}

bool test_oklab_to_lin()
{
    bool ok = true;
    // In-gamut inputs: the q15 Oklab of linear colours, by the float code.
    for(int kind = 0; kind < 2; kind++)
    {
        stat             st[3] = {{"r"}, {"g"}, {"b"}};
        std::vector<q15> L, A, B;
        auto             push = [&](float r, float g, float b) {
            const float c[3] = {r, g, b};
            float       o[3];
            lin_to_oklab(c, o);
            L.push_back(q15_from(o[0])), A.push_back(q15_from(o[1])), B.push_back(q15_from(o[2]));
            if(L.size() == 1 << 16) run_oklab_to_lin(L, A, B, st), L.clear(), A.clear(), B.clear();
        };
        if(kind == 0)
            for(int x = 0; x < 256; x++)
                for(int y = 0; y < 256; y++)
                    for(int z = 0; z < 256; z++) push(s_byte_lin[x], s_byte_lin[y], s_byte_lin[z]);
        else
            for(int k = 0; k < 4 << 20; k++)
                push((rnd() & 32767) / 32768.0f, (rnd() & 32767) / 32768.0f, (rnd() & 32767) / 32768.0f);
        run_oklab_to_lin(L, A, B, st);
        printf("Oklab to lin, the q15 Oklab of %s\n",
               kind ? "4M random linear colours" : "every sRGB byte colour (the byte to linear table of tones.cpp)");
        for(const stat& c : st)
        {
            c.print();
            ok = ok && c.max <= 8.0;
        }
    }
    // Out of gamut: any L, a, b. Reported, not a target. The float result is held to 0 to 1.
    stat             st[3] = {{"r"}, {"g"}, {"b"}};
    std::vector<q15> L, A, B;
    for(int k = 0; k < 1 << 20; k++)
        L.push_back(static_cast<q15>(rnd())), A.push_back(static_cast<q15>(rnd())), B.push_back(static_cast<q15>(rnd()));
    run_oklab_to_lin(L, A, B, st);
    printf("Oklab to lin, 1M random q15 L, a, b (mostly out of gamut, not a target)\n");
    for(const stat& c : st) c.print();
    return ok;
}
} // namespace

int main()
{
    for(int v = 0; v < 256; v++)
    {
        const float c = v / 255.0f;
        s_byte_lin[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    const bool c1 = test_cbrt();
    const bool c2 = test_lin_to_oklab();
    const bool c3 = test_oklab_to_lin();
    printf("targets: cube root %s, lin to Oklab %s, Oklab to lin %s\n", c1 ? "met" : "MISSED", c2 ? "met" : "MISSED",
           c3 ? "met" : "MISSED");
    return c1 && c2 && c3 ? 0 : 1;
}
