// Host test of tone_fx_search (main/tone_fx_search.h, as its stand in in
// ../model/tone_fx_search_model.cpp) against a float scan of all entries with
// the distance of main/tones_float_ref.inc. Prints one line per point set and exits 1 when a check fails.
//
// Checks per point. The point p is float. The kernel gets p rounded to q15.
//   best: when the kernel's best entry r differs from the float best t, d(p, r) - d(p, t) must not
//         exceed 1e-6 + q(r) + q(t). q(e) is the quantisation of entry e at p: the float distance
//         d(p, e) against the exact distance of the q15 point to the q15 entry.
//   next: the same for the kernel's next against the float second nearest.
//   kernel: on the q15 values, the best and next distances may exceed the two smallest by less
//         than 128 counts squared (the rounding of the key).
//   model: best and next equal the first two entries ordered by (saturated distance rounded down
//         to 128, entry index), computed here without the lanes and the two minima.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "tone_fx_search.h"
#include "tonemap.h"

namespace
{
const int    N       = static_cast<int>(TONEMAP_COUNT);
const double TOL     = 1e-6;
const double COUNTS2 = 1073741824.0; // 2^30, q15 counts squared per unit

float lab[N][3];
int   qe[N][3];  // entry L, 2a, 2b in q15, as the kernel's table holds them
int   qmax[3];   // the kernel's maxima of the point's L, 2A and 2B

// dist() of main/tones_float_ref.inc
float dist(const float* a, const float* b)
{
    float dl = a[0] - b[0];
    float da = (a[1] - b[1]) * TONEMAP_CHROMA_WEIGHT;
    float db = (a[2] - b[2]) * TONEMAP_CHROMA_WEIGHT;
    return dl * dl + da * da + db * db;
}

double ddist(const float* p, int e)
{
    const double dl = static_cast<double>(p[0]) - lab[e][0];
    const double da = (static_cast<double>(p[1]) - lab[e][1]) * TONEMAP_CHROMA_WEIGHT;
    const double db = (static_cast<double>(p[2]) - lab[e][2]) * TONEMAP_CHROMA_WEIGHT;
    return dl * dl + da * da + db * db;
}

// Exact distance of the q15 point (L, 2A, 2B) to entry e, counts squared, nothing saturated.
int64_t qdist(const int* q, int e)
{
    int64_t s = 0;
    for(int k = 0; k < 3; k++) s += static_cast<int64_t>(q[k] - qe[e][k]) * (q[k] - qe[e][k]);
    return s;
}

int sat16(int v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

// The kernel's distance: the point saturated to 16 bits and held at qmax, differences saturated to
// 16 bits, the sum held at 2^31 - 1, then rounded down to 128.
int64_t kdist(const int* q, int e)
{
    int64_t s = 0;
    for(int k = 0; k < 3; k++)
    {
        const int     p = std::min(sat16(q[k]), qmax[k]);
        const int64_t d = sat16(qe[e][k] - p);
        s += d * d;
    }
    if(s > INT32_MAX) s = INT32_MAX;
    return s & ~static_cast<int64_t>(127);
}

struct stats
{
    const char* name;
    long        n = 0, best_diff = 0, next_diff = 0, fail_best = 0, fail_next = 0, kernel_bad = 0, model_bad = 0;
    double      max_ex_best = 0, sum_ex_best = 0, max_ex_next = 0, sum_ex_next = 0;
    double      max_beyond_q = -1; // largest excess minus its quantisation, over best and next
};

void run(stats& st, const float p[3])
{
    const q15 L = q15_from(p[0]), A = q15_from(p[1]), B = q15_from(p[2]);
    uint8_t   kb, kn;
    tone_fx_search(L, A, B, &kb, &kn);
    st.n++;

    // float scan, ties to the lower index, as the candidate loop of main/tones_float_ref.inc
    int   tb = 0, tn = 0;
    float bd = 1e30f, nd = 1e30f;
    for(int e = 0; e < N; e++)
    {
        const float d = dist(p, lab[e]);
        if(d < bd) nd = bd, tn = tb, bd = d, tb = e;
        else if(d < nd) nd = d, tn = e;
    }

    const int q[3] = {L, 2 * A, 2 * B};
    auto      qerr = [&](int e) { return std::fabs(ddist(p, e) - static_cast<double>(qdist(q, e)) / COUNTS2); };
    auto      judge = [&](int got, int want, long& diff, long& fail, double& mx, double& sum) {
        if(got == want) return;
        diff++;
        const double ex = static_cast<double>(dist(p, lab[got])) - static_cast<double>(dist(p, lab[want]));
        const double qq = qerr(got) + qerr(want);
        if(ex > mx) mx = ex;
        sum += ex;
        if(ex - qq > st.max_beyond_q) st.max_beyond_q = ex - qq;
        if(ex > TOL + qq)
        {
            if(fail < 3)
                printf("  FAIL %s p %.6f %.6f %.6f: got %d (%.9f) want %d (%.9f), quantisation %.3g\n", st.name,
                       static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2]), got,
                       static_cast<double>(dist(p, lab[got])), want, static_cast<double>(dist(p, lab[want])), qq);
            fail++;
        }
    };
    judge(kb, tb, st.best_diff, st.fail_best, st.max_ex_best, st.sum_ex_best);
    judge(kn, tn, st.next_diff, st.fail_next, st.max_ex_next, st.sum_ex_next);

    // the two smallest exact q15 distances, ties to the lower index
    int64_t b1 = INT64_MAX, b2 = INT64_MAX;
    // the first two by (kernel distance, index)
    int     m1 = -1, m2 = -1;
    int64_t k1 = INT64_MAX, k2 = INT64_MAX;
    for(int e = 0; e < N; e++)
    {
        const int64_t d = qdist(q, e);
        if(d < b1) b2 = b1, b1 = d;
        else if(d < b2) b2 = d;
        const int64_t k = kdist(q, e);
        if(k < k1) k2 = k1, m2 = m1, k1 = k, m1 = e;
        else if(k < k2) k2 = k, m2 = e;
    }
    if(kb == kn || qdist(q, kb) - b1 >= 128 || qdist(q, kn) - b2 >= 128)
    {
        if(st.kernel_bad < 3)
            printf("  KERNEL %s L %d A %d B %d: best %d +%lld, next %d +%lld counts squared\n", st.name, L, A, B, kb,
                   static_cast<long long>(qdist(q, kb) - b1), kn, static_cast<long long>(qdist(q, kn) - b2));
        st.kernel_bad++;
    }
    if(kb != m1 || kn != m2) st.model_bad++;
}

void report(const stats& st)
{
    char beyond[32] = "none";
    if(st.best_diff || st.next_diff) snprintf(beyond, sizeof(beyond), "%.3g", st.max_beyond_q);
    printf("%-10s %7ld points | best differs %6ld (%.4f%%) max %.3g mean %.3g | next differs %6ld (%.4f%%) "
           "max %.3g mean %.3g | beyond quantisation max %s | fail %ld %ld kernel %ld model %ld\n",
           st.name, st.n, st.best_diff, 100.0 * st.best_diff / st.n, st.max_ex_best,
           st.best_diff ? st.sum_ex_best / st.best_diff : 0.0, st.next_diff, 100.0 * st.next_diff / st.n,
           st.max_ex_next, st.next_diff ? st.sum_ex_next / st.next_diff : 0.0, beyond, st.fail_best, st.fail_next,
           st.kernel_bad, st.model_bad);
}

uint32_t s_rng = 0x6d2b79f5u;

float uniform(float lo, float hi)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return lo + (hi - lo) * static_cast<float>(s_rng >> 8) * (1.0f / 16777216.0f);
}
} // namespace

int main()
{
    float amin = 1, amax = -1, bmin = 1, bmax = -1;
    for(int e = 0; e < N; e++)
    {
        lab[e][0] = k_tonemap[e].L, lab[e][1] = k_tonemap[e].a, lab[e][2] = k_tonemap[e].b;
        qe[e][0]  = q15_from(k_tonemap[e].L);
        qe[e][1]  = q15_from(TONEMAP_CHROMA_WEIGHT * k_tonemap[e].a);
        qe[e][2]  = q15_from(TONEMAP_CHROMA_WEIGHT * k_tonemap[e].b);
        amin = std::fmin(amin, lab[e][1]), amax = std::fmax(amax, lab[e][1]);
        bmin = std::fmin(bmin, lab[e][2]), bmax = std::fmax(bmax, lab[e][2]);
    }
    for(int k = 0; k < 3; k++)
    {
        qmax[k] = 32767;
        for(int e = 0; e < N; e++) qmax[k] = std::min(qmax[k], 32767 + qe[e][k]);
    }
    printf("point held at L %.4f A %.4f B %.4f\n", qmax[0] / 32768.0, qmax[1] / 65536.0, qmax[2] / 65536.0);

    std::vector<stats> all;
    stats              st;

    st = stats{"entries"};
    for(int e = 0; e < N; e++) run(st, lab[e]);
    all.push_back(st);

    st = stats{"midpoints"};
    for(int e = 0; e < N; e++)
        for(int f = e + 1; f < N; f++)
            if(dist(lab[e], lab[f]) < 0.05f * 0.05f)
            {
                const float p[3] = {(lab[e][0] + lab[f][0]) / 2, (lab[e][1] + lab[f][1]) / 2,
                                    (lab[e][2] + lab[f][2]) / 2};
                run(st, p);
            }
    all.push_back(st);

    // every q15 step of L from -0.02 to the top of q15
    st = stats{"gray"};
    for(int l = -655; l <= 32767; l++)
    {
        const float p[3] = {l / 32768.0f, 0.0f, 0.0f};
        run(st, p);
    }
    all.push_back(st);

    // the table's range with a margin of 0.05 in L and 0.1 in a and b
    st = stats{"near"};
    for(int i = 0; i < 200000; i++)
    {
        const float p[3] = {uniform(-0.05f, 1.0f), uniform(amin - 0.1f, amax + 0.1f), uniform(bmin - 0.1f, bmax + 0.1f)};
        run(st, p);
    }
    all.push_back(st);

    // Oklab a and b of the sRGB gamut, most of it far from every entry
    st = stats{"wide"};
    for(int i = 0; i < 50000; i++)
    {
        const float p[3] = {uniform(-0.05f, 1.0f), uniform(-0.25f, 0.3f), uniform(-0.32f, 0.2f)};
        run(st, p);
    }
    all.push_back(st);

    stats tot{"all"};
    for(const stats& s : all)
    {
        report(s);
        tot.n += s.n, tot.best_diff += s.best_diff, tot.next_diff += s.next_diff;
        tot.fail_best += s.fail_best, tot.fail_next += s.fail_next;
        tot.kernel_bad += s.kernel_bad, tot.model_bad += s.model_bad;
        tot.sum_ex_best += s.sum_ex_best, tot.sum_ex_next += s.sum_ex_next;
        tot.max_ex_best  = std::fmax(tot.max_ex_best, s.max_ex_best);
        tot.max_ex_next  = std::fmax(tot.max_ex_next, s.max_ex_next);
        tot.max_beyond_q = std::fmax(tot.max_beyond_q, s.max_beyond_q);
    }
    report(tot);
    const bool ok = tot.fail_best == 0 && tot.fail_next == 0 && tot.kernel_bad == 0 && tot.model_bad == 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
