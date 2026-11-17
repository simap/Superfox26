// Oklab kernel probe. Prints the vector unit results the kernels rest on (lines OKLAB SEM, each with
// the value the TRM gives), runs the self test (every kernel of main/tone_fx_oklab.cpp against its
// stand in, named *_model in this build) and prints the mismatch count, then OKLAB PROBE DONE.
#include <cstdint>
#include <cstdio>

#include "tone_fx_model.h"
#include "tone_fx_oklab.h"

static void print8(const char* what, const int16_t* v, const char* trm)
{
    printf("OKLAB SEM %s:", what);
    for(int i = 0; i < 8; i++) printf(" %d", v[i]);
    printf("  (TRM: %s)\n", trm);
}

static void semantics()
{
    alignas(16) int16_t a[8] = {-32768, -32768, -32767, -30000, 32767, 32767, 3, -3};
    alignas(16) int16_t b[8] = {0, -1, -1, -10000, 0, 1, 5, 5};
    alignas(16) int16_t o[8];
    const int16_t*      pa = a;
    const int16_t*      pb = b;
    int16_t*            po = o;
    // The kernels give no output that depends on the low bound of vadds and vsubs.
    __asm__ volatile("ee.vld.128.ip q0, %[pa], 0\n ee.vld.128.ip q1, %[pb], 0\n ee.vadds.s16 q2, q0, q1\n"
                     "ee.vst.128.ip q2, %[po], 0\n"
                     : [pa] "+r"(pa), [pb] "+r"(pb), [po] "+r"(po)::"memory");
    print8("vadds.s16 a+b", o, "-32768 -32768 -32768 -32768 32767 32767 8 2");
    // vmul.s16 at SAR 1: floor of the shifted product, low 16 bits kept.
    int t;
    __asm__ volatile("movi %[t], 1\n wsr.sar %[t]\n ee.vld.128.ip q0, %[pa], 0\n ee.vld.128.ip q1, %[pb], 0\n"
                     "ee.vmul.s16 q2, q0, q0\n ee.vst.128.ip q2, %[po], 0\n"
                     : [pa] "+r"(pa), [pb] "+r"(pb), [po] "+r"(po), [t] "=&r"(t)::"memory");
    print8("vmul.s16 a*a>>1", o, "0 0 -32768 29824 -32768 -32768 4 4");
    // Accumulator: a*b four times, then reads at shift 3 and 1 (the first read shifts the
    // accumulator, the second shifts it again), and a sum below -2^31 read at shift 20.
    alignas(16) int16_t c[8] = {-32768, -32768, -32768, -32768, 3, -3, 181, -7};
    alignas(16) int16_t d[8] = {32767, 32767, 32767, 32767, 5, 5, 181, 3};
    alignas(16) int16_t o2[8];
    const int16_t*      pc  = c;
    const int16_t*      pd  = d;
    int16_t*            po2 = o2;
    po                      = o;
    __asm__ volatile("ee.vld.128.ip q0, %[pc], 0\n ee.vld.128.ip q1, %[pd], 0\n ee.zero.qacc\n"
                     "ee.vmulas.s16.qacc q0, q1\n ee.vmulas.s16.qacc q0, q1\n ee.vmulas.s16.qacc q0, q1\n"
                     "ee.vmulas.s16.qacc q0, q1\n movi %[t], 20\n ee.srcmb.s16.qacc q2, %[t], 0\n"
                     "ee.vst.128.ip q2, %[po], 0\n ee.zero.qacc\n ee.vmulas.s16.qacc q0, q1\n"
                     "movi %[t], 3\n ee.srcmb.s16.qacc q3, %[t], 0\n movi %[t], 1\n ee.srcmb.s16.qacc q3, %[t], 0\n"
                     "ee.vst.128.ip q3, %[po2], 0\n"
                     : [pc] "+r"(pc), [pd] "+r"(pd), [po] "+r"(po), [po2] "+r"(po2), [t] "=&r"(t)::"memory");
    print8("qacc 4*c*d >> 20", o, "-4096 -4096 -4096 -4096 0 -1 0 -1");
    print8("qacc c*d >> 3 >> 1", o2, "-32768 -32768 -32768 -32768 0 -1 2047 -2");
    // ee.vmulas.u16.qacc on a non-negative accumulator: 0xFFFF * 0xFFFF read at shift 16.
    alignas(16) int16_t e[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    const int16_t*      pe = e;
    po                     = o;
    __asm__ volatile("ee.vld.128.ip q0, %[pe], 0\n ee.zero.qacc\n ee.vmulas.u16.qacc q0, q0\n"
                     "movi %[t], 17\n ee.srcmb.s16.qacc q2, %[t], 0\n ee.vst.128.ip q2, %[po], 0\n"
                     : [pe] "+r"(pe), [po] "+r"(po), [t] "=&r"(t)::"memory");
    print8("qacc u16 65535*65535 >> 17", o, "32767 x8");
}

namespace
{
uint32_t ok_rand(uint32_t* s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// Inputs of the self test, element k of round j: edge values, then pseudo random ones.
const int16_t k_edges[] = {0, 1, 2, 3, 7, 8, 9, 63, 64, 65, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 16383,
                           16384, 16385, 24575, 24576, 32766, 32767, -1, -2, -16384, -32767, -32768};
const int K_EDGES = static_cast<int>(sizeof(k_edges) / sizeof(k_edges[0]));

// Mismatch count of one kernel and its first mismatch.
struct ok_check
{
    const char* name;
    int         bad = 0;
    int         in[3] = {}, c = 0, p = 0, ch = 0;
    void        cmp(int16_t vc, int16_t vp, int i0, int i1, int i2, int channel)
    {
        if(vc == vp) return;
        if(bad++ == 0) in[0] = i0, in[1] = i1, in[2] = i2, c = vc, p = vp, ch = channel;
    }
    void print() const
    {
        if(bad)
            printf("oklab selftest %s: %d differ, first at input %d %d %d channel %d: stand in %d kernel %d\n", name,
                   bad, in[0], in[1], in[2], ch, c, p);
        else
            printf("oklab selftest %s: 0 differ\n", name);
    }
};

// Every kernel of tone_fx_oklab.h against its stand in over a fixed set of inputs: the cube root on
// every input, the others on all triples of edge values and on pseudo random ones from a fixed
// seed. Prints one line per kernel and returns the number of output elements that differ.
int oklab_selftest()
{
    const int                  N = 512;
    alignas(16) static int16_t in[3][N], out_c[3][N], out_p[3][N];
    uint32_t                   seed = 0x9E3779B9u;
    ok_check                   ck[4] = {{"cbrt"}, {"lin_to_oklab"}, {"lin_to_oklab_1"}, {"oklab_to_lin"}};
    // cube root: every input
    for(int base = -32768; base < 32768; base += N)
    {
        for(int k = 0; k < N; k++) in[0][k] = static_cast<int16_t>(base + k);
        tone_fx_cbrt_model(in[0], N, out_c[0]);
        tone_fx_cbrt(in[0], N, out_p[0]);
        for(int k = 0; k < N; k++) ck[0].cmp(out_c[0][k], out_p[0][k], in[0][k], 0, 0, 0);
    }
    // lin to Oklab and Oklab to lin: all triples of edge values, then 8192 random ones in 0 to 32767
    // and 8192 in 0 to 255
    const int edge_n      = K_EDGES * K_EDGES * K_EDGES;
    const int edge_rounds = (edge_n + N - 1) / N;
    for(int round = 0; round < edge_rounds + 32; round++)
    {
        for(int k = 0; k < N; k++)
        {
            const int e = round * N + k;
            for(int ch = 0; ch < 3; ch++)
            {
                int v;
                if(e < edge_n)
                    v = k_edges[ch == 0 ? e % K_EDGES : ch == 1 ? e / K_EDGES % K_EDGES : e / K_EDGES / K_EDGES];
                else if(round % 2)
                    v = static_cast<int16_t>(ok_rand(&seed) & 32767);
                else
                    v = static_cast<int16_t>(ok_rand(&seed) & 255);
                in[ch][k] = static_cast<int16_t>(v);
            }
        }
        tone_fx_lin_to_oklab_model(in[0], in[1], in[2], N, out_c[0], out_c[1], out_c[2]);
        tone_fx_lin_to_oklab(in[0], in[1], in[2], N, out_p[0], out_p[1], out_p[2]);
        for(int ch = 0; ch < 3; ch++)
            for(int k = 0; k < N; k++) ck[1].cmp(out_c[ch][k], out_p[ch][k], in[0][k], in[1][k], in[2][k], ch);
        for(int k = 0; k < N; k += 7)
        {
            alignas(16) int16_t px[8] = {in[0][k], in[1][k], in[2][k]};
            alignas(16) int16_t lc[8], lp[8];
            tone_fx_lin_to_oklab_1_model(px, lc);
            tone_fx_lin_to_oklab_1(px, lp);
            for(int ch = 0; ch < 3; ch++) ck[2].cmp(lc[ch], lp[ch], px[0], px[1], px[2], ch);
        }
        // Oklab to lin on the Oklab just found (in gamut) and on the raw inputs as L, a, b
        for(int pass = 0; pass < 2; pass++)
        {
            int16_t(*src)[N] = pass ? in : out_c;
            alignas(16) static int16_t oc[3][N], op[3][N];
            tone_fx_oklab_to_lin_model(src[0], src[1], src[2], N, oc[0], oc[1], oc[2]);
            tone_fx_oklab_to_lin(src[0], src[1], src[2], N, op[0], op[1], op[2]);
            for(int ch = 0; ch < 3; ch++)
                for(int k = 0; k < N; k++) ck[3].cmp(oc[ch][k], op[ch][k], src[0][k], src[1][k], src[2][k], ch);
        }
    }
    int total = 0;
    for(const ok_check& c : ck) c.print(), total += c.bad;
    return total;
}
} // namespace

extern "C" void app_main(void)
{
    printf("OKLAB PROBE\n");
    semantics();
    const int bad = oklab_selftest();
    printf("OKLAB SELFTEST %d mismatches\n", bad);
    printf("OKLAB PROBE DONE\n");
}
