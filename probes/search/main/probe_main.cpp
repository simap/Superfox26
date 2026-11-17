// Search kernel probe. It checks the instruction semantics the kernel relies on, builds the table,
// compares the kernel with its stand in (named tone_fx_search_model in this build), prints the
// mismatch count and SEARCH PROBE DONE.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "tone_fx_model.h"
#include "tone_fx_search.h"
#include "tonemap.h"

namespace
{
int s_fail = 0;

// With info set, a difference prints without counting as a failure. That is for the saturation of a
// negative result, which the TRM puts at -2^15 and -2^31 and QEMU at -2^15 + 1 and -2^31 + 1. The
// kernel never produces one.
void expect16(const char* name, const int16_t* got, const int16_t* want, bool info = false)
{
    const bool ok = memcmp(got, want, 16) == 0;
    printf("SEARCH OP %-26s %s:", name, ok ? "ok  " : info ? "info" : "FAIL");
    for(int i = 0; i < 8; i++) printf(" %d", got[i]);
    if(!ok)
    {
        printf(" want");
        for(int i = 0; i < 8; i++) printf(" %d", want[i]);
    }
    printf("\n");
    s_fail += !ok && !info;
}

void expect32(const char* name, const int32_t* got, const int32_t* want, bool info = false)
{
    const bool ok = memcmp(got, want, 16) == 0;
    printf("SEARCH OP %-26s %s:", name, ok ? "ok  " : info ? "info" : "FAIL");
    for(int i = 0; i < 4; i++) printf(" %ld", static_cast<long>(got[i]));
    if(!ok)
    {
        printf(" want");
        for(int i = 0; i < 4; i++) printf(" %ld", static_cast<long>(want[i]));
    }
    printf("\n");
    s_fail += !ok && !info;
}

void vsubs16(const int16_t* x, const int16_t* y, int16_t* out)
{
    __asm__ volatile("ee.vld.128.ip q0, %[x], 0\n ee.vld.128.ip q1, %[y], 0\n ee.vsubs.s16 q2, q0, q1\n"
                     "ee.vst.128.ip q2, %[out], 0\n"
                     : [out] "+r"(out)
                     : [x] "r"(x), [y] "r"(y)
                     : "memory");
}

void vadds32(const int32_t* x, const int32_t* y, int32_t* out)
{
    __asm__ volatile("ee.vld.128.ip q0, %[x], 0\n ee.vld.128.ip q1, %[y], 0\n ee.vadds.s32 q2, q0, q1\n"
                     "ee.vst.128.ip q2, %[out], 0\n"
                     : [out] "+r"(out)
                     : [x] "r"(x), [y] "r"(y)
                     : "memory");
}

// The semantics of each vector instruction the kernel uses, as the stand in models them.
void check_ops()
{
    // Differences the kernel produces, with positive saturation and results down to -32767.
    alignas(16) int16_t x[8]  = {32767, 0, 100, -100, 0, 1, -1, 32767};
    alignas(16) int16_t y[8]  = {-1, -32768, 200, -32768, 32767, -1, 32766, 32767};
    // Differences it never produces, with results of -32768 and below.
    alignas(16) int16_t xn[8] = {-32768, -1, -2, -32768, -32768, -32767, 0, 0};
    alignas(16) int16_t yn[8] = {1, 32767, 32767, 0, 32767, 2, 0, 0};
    alignas(16) int16_t d[8]  = {-32768, 32767, 181, 182, 255, 256, -12345, 1};
    alignas(16) int32_t u[4]  = {1 << 30, INT32_MAX, INT32_MIN, 5};
    alignas(16) int32_t v[4]  = {1 << 30, 1, -1, 6};
    // Sums the kernel produces, of positive terms held at 2^31 - 1.
    alignas(16) int32_t us[4] = {1 << 30, INT32_MAX, 2000000000, 5};
    alignas(16) int32_t vs[4] = {1 << 30, 1, 1073741824, 6};
    alignas(16) int16_t o16[8], o16b[8];
    alignas(16) int32_t o32[4], o32b[4];
    const int16_t       h     = -1234;
    const int16_t*      px    = x;
    const int16_t*      py    = y;
    const int16_t*      pd    = d;
    const int32_t*      pu    = u;
    const int32_t*      pv    = v;

    int16_t want16[8];
    vsubs16(px, py, o16);
    for(int i = 0; i < 8; i++)
    {
        const int s = x[i] - y[i];
        want16[i]   = static_cast<int16_t>(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
    }
    expect16("vsubs.s16 saturates", o16, want16);
    vsubs16(xn, yn, o16);
    for(int i = 0; i < 8; i++)
    {
        const int s = xn[i] - yn[i];
        want16[i]   = static_cast<int16_t>(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
    }
    expect16("vsubs.s16 to -32768, TRM", o16, want16, true);

    int16_t* po = o16;
    __asm__ volatile("ssai 0\n ee.vld.128.ip q0, %[pd], 0\n ee.vmul.s16 q1, q0, q0\n ee.vst.128.ip q1, %[po], 0\n"
                     : [po] "+r"(po)
                     : [pd] "r"(pd)
                     : "memory");
    for(int i = 0; i < 8; i++) want16[i] = static_cast<int16_t>(static_cast<uint32_t>(d[i] * d[i]) & 0xffffu);
    expect16("vmul.s16 sar 0 low half", o16, want16);

    po = o16;
    __asm__ volatile("ssai 16\n ee.vld.128.ip q0, %[pd], 0\n ee.vmul.s16 q1, q0, q0\n ee.vst.128.ip q1, %[po], 0\n"
                     "ssai 0\n"
                     : [po] "+r"(po)
                     : [pd] "r"(pd)
                     : "memory");
    for(int i = 0; i < 8; i++) want16[i] = static_cast<int16_t>((d[i] * d[i]) >> 16);
    expect16("vmul.s16 sar 16 high half", o16, want16);

    // The two halves zipped back into 32 bit squares.
    int32_t* p32  = o32;
    int32_t* p32b = o32b;
    __asm__ volatile("ssai 0\n ee.vld.128.ip q0, %[pd], 0\n ee.vmul.s16 q1, q0, q0\n ssai 16\n"
                     "ee.vmul.s16 q2, q0, q0\n ssai 0\n ee.vzip.16 q1, q2\n"
                     "ee.vst.128.ip q1, %[p32], 0\n ee.vst.128.ip q2, %[p32b], 0\n"
                     : [p32] "+r"(p32), [p32b] "+r"(p32b)
                     : [pd] "r"(pd)
                     : "memory");
    int32_t want32[4], want32b[4];
    for(int i = 0; i < 4; i++) want32[i] = d[i] * d[i], want32b[i] = d[i + 4] * d[i + 4];
    expect32("vzip.16 squares 0..3", o32, want32);
    expect32("vzip.16 squares 4..7", o32b, want32b);

    vadds32(us, vs, o32);
    for(int i = 0; i < 4; i++)
    {
        const int64_t s = static_cast<int64_t>(us[i]) + vs[i];
        want32[i]       = static_cast<int32_t>(s > INT32_MAX ? INT32_MAX : s);
    }
    expect32("vadds.s32 saturates", o32, want32);
    vadds32(u, v, o32);
    for(int i = 0; i < 4; i++)
    {
        const int64_t s = static_cast<int64_t>(u[i]) + v[i];
        want32[i]       = static_cast<int32_t>(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s);
    }
    expect32("vadds.s32 to -2^31, TRM", o32, want32, true);

    p32  = o32;
    p32b = o32b;
    __asm__ volatile("ee.vld.128.ip q0, %[pu], 0\n ee.vld.128.ip q1, %[pv], 0\n ee.vmin.s32 q2, q0, q1\n"
                     "ee.vmax.s32 q3, q0, q1\n ee.vst.128.ip q2, %[p32], 0\n ee.vst.128.ip q3, %[p32b], 0\n"
                     : [p32] "+r"(p32), [p32b] "+r"(p32b)
                     : [pu] "r"(pu), [pv] "r"(pv)
                     : "memory");
    for(int i = 0; i < 4; i++) want32[i] = u[i] < v[i] ? u[i] : v[i], want32b[i] = u[i] < v[i] ? v[i] : u[i];
    expect32("vmin.s32", o32, want32);
    expect32("vmax.s32", o32b, want32b);

    p32  = o32;
    p32b = o32b;
    __asm__ volatile("ee.vld.128.ip q0, %[pu], 0\n ee.vld.128.ip q1, %[pv], 0\n ee.andq q2, q0, q1\n"
                     "ee.orq q3, q0, q1\n ee.vst.128.ip q2, %[p32], 0\n ee.vst.128.ip q3, %[p32b], 0\n"
                     : [p32] "+r"(p32), [p32b] "+r"(p32b)
                     : [pu] "r"(pu), [pv] "r"(pv)
                     : "memory");
    for(int i = 0; i < 4; i++) want32[i] = u[i] & v[i], want32b[i] = u[i] | v[i];
    expect32("andq", o32, want32);
    expect32("orq", o32b, want32b);

    po                = o16b;
    const int16_t* ph = &h;
    __asm__ volatile("ee.vldbc.16 q0, %[ph]\n ee.vst.128.ip q0, %[po], 0\n"
                     : [po] "+r"(po)
                     : [ph] "r"(ph)
                     : "memory");
    for(int i = 0; i < 8; i++) want16[i] = h;
    expect16("vldbc.16 broadcast", o16b, want16);
    printf("SEARCH OP checks failed %d\n", s_fail);
}

uint32_t xorshift(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// A q15 in lo to hi from the generator.
q15 draw(uint32_t& s, int lo, int hi)
{
    return static_cast<q15>(lo + static_cast<int>(xorshift(s) % static_cast<uint32_t>(hi - lo + 1)));
}

// The kernel and its stand in over every entry, edge values and 4096 pseudo random points from a
// fixed seed. Returns the number of points where best or next differ and prints the first few. The
// table is built.
int search_selftest()
{
    int  bad   = 0;
    auto check = [&bad](q15 L, q15 A, q15 B) {
        uint8_t cb, cn, pb, pn;
        tone_fx_search_model(L, A, B, &cb, &cn);
        tone_fx_search(L, A, B, &pb, &pn);
        if(cb == pb && cn == pn) return;
        if(bad < 4)
            printf("search selftest: L %d A %d B %d: stand in %u %u, kernel %u %u\n", L, A, B,
                   static_cast<unsigned>(cb), static_cast<unsigned>(cn), static_cast<unsigned>(pb),
                   static_cast<unsigned>(pn));
        bad++;
    };
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
        check(q15_from(k_tonemap[e].L), q15_from(k_tonemap[e].a), q15_from(k_tonemap[e].b));
    static const q15 edge[] = {-32768, -16384, -1, 0, 1, 16384, 32767};
    for(q15 L : edge)
        for(q15 A : edge)
            for(q15 B : edge) check(L, A, B);
    uint32_t s = 0x2545f491u;
    for(int i = 0; i < 2048; i++) check(draw(s, -32768, 32767), draw(s, -32768, 32767), draw(s, -32768, 32767));
    // Near the entries, with L a little past both ends and a and b a little past the table's.
    for(int i = 0; i < 2048; i++) check(draw(s, -1000, 32767), draw(s, -4000, 9000), draw(s, -4000, 6000));
    return bad;
}
} // namespace

extern "C" void app_main(void)
{
    printf("SEARCH PROBE start, %d entries\n", static_cast<int>(TONEMAP_COUNT));
    check_ops();
    // A few points: the paper entry, the point between the two entries at L 0.9875 and 0.9870,
    // the red reference, and a point far from every entry.
    static const q15 pts[][3] = {{32767, 0, 0}, {32350, 0, 0}, {15516, 5220, 2736}, {16384, -8192, -9830}};
    for(const auto& p : pts)
    {
        uint8_t cb, cn, pb, pn;
        tone_fx_search_model(p[0], p[1], p[2], &cb, &cn);
        tone_fx_search(p[0], p[1], p[2], &pb, &pn);
        printf("SEARCH L %6d A %6d B %6d: stand in best %3u next %3u, kernel best %3u next %3u\n", p[0], p[1], p[2],
               static_cast<unsigned>(cb), static_cast<unsigned>(cn), static_cast<unsigned>(pb),
               static_cast<unsigned>(pn));
    }
    printf("SEARCH SELFTEST mismatches %d\n", search_selftest());
    printf("SEARCH PROBE DONE\n");
}
