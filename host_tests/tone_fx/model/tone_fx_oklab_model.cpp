// The stand ins of the Oklab kernels (main/tone_fx_oklab.h) for host builds and the probes. Each
// models the vector unit lane by lane: the same operations in the same order with the same shifts,
// rounding and saturation as main/tone_fx_oklab.cpp, so that it gives the same bits. The models of
// the vector operations are the op_* helpers and qacc below. The constants are those of
// main/tone_fx_oklab_internal.h, and the maths is described in main/tone_fx_oklab.cpp.

#include "tone_fx_model.h"
#include "tone_fx_oklab.h"
#include "tone_fx_oklab_internal.h"

namespace
{
using namespace tone_fx_oklab_internal;

// ---------------------------------------------------------------- models of the vector operations

// ee.vadds.s16 and ee.vsubs.s16: saturating.
inline int16_t op_add(int a, int b)
{
    const int s = a + b;
    return static_cast<int16_t>(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}
inline int16_t op_sub(int a, int b)
{
    return op_add(a, -b);
}
// ee.vmul.s16: the 32 bit product shifted right arithmetically by SAR (floor), low 16 bits kept.
inline int16_t op_mul(int16_t a, int16_t b, int sar)
{
    return static_cast<int16_t>(static_cast<uint16_t>((static_cast<int32_t>(a) * b) >> sar));
}
// ee.vmul.u16: unsigned 16 bit product shifted right logically by SAR, low 16 bits kept.
inline int16_t op_mulu(int16_t a, int16_t b, int sar)
{
    const uint32_t p = static_cast<uint32_t>(static_cast<uint16_t>(a)) * static_cast<uint16_t>(b);
    return static_cast<int16_t>(static_cast<uint16_t>(p >> sar));
}
// ee.vcmp.lt.s16 and ee.vcmp.gt.s16: all ones where true.
inline int16_t op_lt(int16_t a, int16_t b)
{
    return a < b ? -1 : 0;
}
inline int16_t op_gt(int16_t a, int16_t b)
{
    return a > b ? -1 : 0;
}
inline int16_t op_min(int16_t a, int16_t b)
{
    return a < b ? a : b;
}
inline int16_t op_max(int16_t a, int16_t b)
{
    return a > b ? a : b;
}
inline int16_t op_and(int16_t a, int16_t b)
{
    return static_cast<int16_t>(a & b);
}
inline int16_t op_xor(int16_t a, int16_t b)
{
    return static_cast<int16_t>(a ^ b);
}

// One lane of the 40 bit accumulator. ee.vmulas.*.qacc adds a product and saturates to 40 bits.
// ee.srcmb.s16.qacc shifts it right arithmetically (floor), keeps the shifted value and returns it
// saturated to 16 bits.
struct qacc
{
    int64_t v = 0;
    void    add(int64_t p)
    {
        const int64_t lim = (int64_t(1) << 39) - 1;
        v += p;
        v = v > lim ? lim : v < -lim - 1 ? -lim - 1 : v;
    }
    void mulas(int16_t a, int16_t b)
    {
        add(static_cast<int64_t>(a) * b);
    }
    // ee.vmulas.u16.qacc reads the accumulator as unsigned 40 bits and saturates to 2^40 - 1. The
    // kernels call it on a non-negative accumulator only.
    void mulasu(int16_t a, int16_t b)
    {
        const int64_t  top = int64_t(1) << 40;
        const uint64_t u   = static_cast<uint64_t>(v < 0 ? v + top : v) +
                           static_cast<uint64_t>(static_cast<uint16_t>(a)) * static_cast<uint16_t>(b);
        const int64_t s = static_cast<int64_t>(u > static_cast<uint64_t>(top - 1) ? top - 1 : u);
        v               = s >= top / 2 ? s - top : s;
    }
    int16_t srcmb(int s)
    {
        v >>= s;
        return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
};

// ---------------------------------------------------------------- lane models

// The core. d = x'' - 24576, mA all ones when r >= 1, mB all ones when r = 2, S = 2^(14 - q) or 0
// for a zero input. Returns the cube root read out as the path says.
int16_t core_lane(int16_t d, int16_t mA, int16_t mB, int16_t S, const core_path& k)
{
    int16_t       t  = op_add(d, d);
    t                = op_add(t, 1);
    t                = op_add(t, t); // 4 * d + 2
    const int16_t F  = op_add(op_add(CORE_F0, op_and(mA, CORE_F1 - CORE_F0)), op_and(mB, CORE_F2 - CORE_F1));
    const int16_t G  = op_add(op_add(CORE_G0, op_and(mA, CORE_G1 - CORE_G0)), op_and(mB, CORE_G2 - CORE_G1));
    const int16_t t2 = op_mul(t, t, 15);
    const int16_t t3 = op_mul(t2, t, 15);
    const int16_t t4 = op_mul(t2, t2, 15);
    const int16_t t5 = op_mul(t4, t, 15);
    qacc          a;
    a.mulas(t, k.c1);
    a.mulas(t2, k.c2);
    a.mulas(t3, k.c3);
    a.mulas(t4, k.c4);
    a.mulas(t5, k.c5);
    a.mulas(k.kd_a, k.kd_b);
    const int16_t D  = a.srcmb(15);
    const int16_t DF = op_mul(D, F, 15);
    qacc          o;
    o.mulas(DF, S);
    o.srcmb(3);
    o.mulas(G, S);
    o.mulas(k.ko_a, k.ko_b);
    return o.srcmb(k.out_shift);
}

// x'' by four steps that each multiply x by 8 where it is below 4096 (8x = x + 7x, the product
// 7x at SAR 0 wraps in the lanes that do not take it) and double S where it is not, then two that
// double x where it is below 8192 and below 16384. S starts at 1024, or 0 for x = 0.
int16_t cbrt_lane(int16_t x)
{
    x       = op_max(x, 0);
    int16_t S = op_and(1024, op_gt(x, 0));
    for(int k = 0; k < 4; k++)
    {
        const int16_t m  = op_lt(x, 4096);
        const int16_t x7 = op_mul(x, 7, 0);
        const int16_t nm = op_gt(x, 4095);
        x                = op_add(x, op_and(x7, m));
        S                = op_add(S, op_and(S, nm));
    }
    const int16_t mA = op_lt(x, 16384), mB = op_lt(x, 8192);
    x                = op_add(x, op_and(x, mB));
    x                = op_add(x, op_and(x, mA));
    return core_lane(op_sub(x, 24576), mA, mB, S, PATH_EXACT);
}

// The cube root of acc / 2^30, offset q16 out, 0 (w = -32768) for acc <= 0. The reads go e = 0,
// -1 to -4 (doublings of the read at 0), then 1 to 15. f = v XOR 0xC000, y = min f, m = all ones
// where the read is below 16384. j = 15 - e of the pick is the count of m, and r = j mod 3 is that
// count with each bottom level of a group of three (e = 13, 10, 7, 4, 1, -2) counted -2 in place
// of 1, so cnt = -r. S starts at 16384 and loses 2^(13 - g) at the bottom level of group g that is
// below 16384, and its last 256 at e = -4, which is below 16384 only for acc < 1024 (no LMS of an
// in-range input lies in 1 to 1023).
int16_t lms_cbrt_lane(qacc a)
{
    const int16_t K = static_cast<int16_t>(0xC000);
    int16_t       v = a.srcmb(0);
    int16_t       f = op_xor(v, K);
    int16_t       y = f;
    int16_t       m = op_gt(f, -16385);
    int16_t       cnt = m, S = 16384;
    for(int e = -1; e >= -4; e--)
    {
        v = op_add(v, v);
        f = op_xor(v, K);
        y = op_min(y, f);
        m = op_gt(f, -16385);
        if(e == -2)
        {
            cnt = op_sub(op_sub(cnt, m), m);
            S   = op_sub(S, op_and(m, 256));
        }
        else
            cnt = op_add(cnt, m);
        if(e == -4) S = op_sub(S, op_and(m, 256));
    }
    for(int e = 1; e <= 15; e++)
    {
        v = a.srcmb(1);
        f = op_xor(v, K);
        y = op_min(y, f);
        m = op_gt(f, -16385);
        if(e % 3 == 1)
        {
            cnt = op_sub(op_sub(cnt, m), m);
            S   = op_sub(S, op_and(m, static_cast<int16_t>(512 << ((e - 1) / 3))));
        }
        else
            cnt = op_add(cnt, m);
    }
    const int16_t d  = op_add(y, 24576);
    const int16_t mA = op_lt(cnt, 0), mB = op_lt(cnt, -1);
    return core_lane(d, mA, mB, S, PATH_FLOOR);
}

void lin_to_oklab_lane(int16_t r, int16_t g, int16_t b, int16_t* L, int16_t* A, int16_t* B)
{
    int16_t w[3];
    for(int i = 0; i < 3; i++)
    {
        qacc a;
        a.mulas(r, M1[i][0]);
        a.mulas(g, M1[i][1]);
        a.mulas(b, M1[i][2]);
        w[i] = lms_cbrt_lane(a);
    }
    // w = cbrt * 65536 - 32768. A row with coefficients C sums C * w = 2^31 * out - 2^15 * sum C, so
    // the L row adds 2^30 = -32768 * -32768 before the read out at shift 16, and a and b, whose rows
    // sum to 0, add none. Every row adds the rounding half 2^15 = 128 * 256.
    qacc l;
    for(int i = 0; i < 3; i++) l.mulas(w[i], M2_L[i]);
    l.mulas(128, 256);
    l.mulas(-32768, -32768);
    *L = l.srcmb(16);
    qacc bb;
    for(int i = 0; i < 3; i++) bb.mulas(w[i], M2_B[i]);
    bb.mulas(128, 256);
    *B = bb.srcmb(16);
    qacc aa;
    aa.mulas(w[0], M2_A_L[0]);
    aa.mulas(w[0], M2_A_L[1]);
    for(int k = 0; k < 3; k++) aa.mulas(w[1], M2_A_M[k]);
    aa.mulas(w[2], M2_A_S);
    aa.mulas(128, 256);
    *A = aa.srcmb(16);
}

// One cube of oklab_to_lin. w = round(l * 65536) - 32768 and e3 = 3 * floor(eps * 2^13), eps the
// rounding residue of w in units of 2^-16, eps in -1/2 to 1/2. With u = w + 32768 unsigned the cube
// is (u^3 + 3 u^2 eps) / 2^32 in q16. u * u is taken in halves, ph = u * u >> 16 and pl its low 16
// bits. The accumulator takes pl * u, shifts by 4, adds (ph / 2) * e3 = 3 u^2 eps / 16 and 2^29,
// shifts by 12, adds ph * u and 2 * -32768 * 32767 = -2^31 + 2^16, and is read out at shift 16. The
// 2^29 keeps the accumulator positive for the unsigned multiply that follows it, and returns as
// 2^17 at the last read. The result is floor(cube * 65536 + 3) - 32768, which is cube * 65536 -
// 32768 + 5/2 on average. The rows of the last matrix take the 5/2 out.
int16_t cube_lane(int16_t w, int16_t e3)
{
    const int16_t u  = op_xor(w, static_cast<int16_t>(0x8000));
    const int16_t ph = op_mulu(u, u, 16);
    const int16_t h  = op_mulu(ph, static_cast<int16_t>(0x8000), 16);
    const int16_t pl = op_mul(u, u, 0);
    qacc          a;
    a.mulasu(pl, u);
    a.srcmb(4);
    a.mulas(h, e3);
    a.mulas(-32768, -16384);
    a.srcmb(12);
    a.mulasu(ph, u);
    a.mulas(-32768, 32767);
    a.mulas(-32768, 32767);
    return a.srcmb(16);
}

// One of l, m, s: the row of the first matrix at 2^30 per unit. c selects the row.
qacc lms_row(int16_t L, int16_t A, int16_t B, int c)
{
    qacc q;
    q.mulas(L, 16384);
    q.mulas(L, 16384);
    if(c == 0) q.mulas(A, M3_LA), q.mulas(B, M3_LB);
    if(c == 1) q.mulas(A, M3_MA), q.mulas(B, M3_MB);
    if(c == 2) q.mulas(A, M3_SA), q.mulas(B, M3_SB0), q.mulas(B, M3_SB1);
    return q;
}

void oklab_to_lin_lane(int16_t L, int16_t A, int16_t B, int16_t* r, int16_t* g, int16_t* b)
{
    int16_t c[3];
    for(int k = 0; k < 3; k++)
    {
        // Offset q16 read out with -2^29 + 2^13, rounded: w = round(l * 65536) - 32768.
        qacc q = lms_row(L, A, B, k);
        q.mulas(-32640, 16448);
        const int16_t w = q.srcmb(14);
        // The rounding residue of w, recomputed: (l * 65536 - 32768 - w) * 2^14, read out at shift
        // 1, times 3.
        qacc e = lms_row(L, A, B, k);
        e.mulas(-32768, 16384);
        e.mulas(w, -16384);
        const int16_t e2 = e.srcmb(1);
        c[k]             = cube_lane(w, op_add(op_add(e2, e2), e2));
    }
    const int16_t cl = c[0], cm = c[1], cs = c[2];
    // The cubes c hold cube * 65536 - 32768 + 5/2 on average. A row with coefficients C (sum 32768)
    // sums 2^31 * out - (32768 - 5/2) * 32768, so it adds 2^30 - 81920 and the rounding half 2^15.
    qacc qr, qg, qb;
    qr.mulas(cl, M4_RL0), qr.mulas(cl, M4_RL0), qr.mulas(cl, M4_RL1), qr.mulas(cl, M4_RL1), qr.mulas(cl, M4_RL1);
    qr.mulas(cm, M4_RM0), qr.mulas(cm, M4_RM0), qr.mulas(cm, M4_RM0), qr.mulas(cm, M4_RM1);
    qr.mulas(cs, M4_RS);
    qg.mulas(cl, M4_GL), qg.mulas(cl, M4_GL);
    qg.mulas(cm, M4_GM0), qg.mulas(cm, M4_GM0), qg.mulas(cm, M4_GM1);
    qg.mulas(cs, M4_GS);
    qb.mulas(cl, M4_BL), qb.mulas(cm, M4_BM), qb.mulas(cs, M4_BS0), qb.mulas(cs, M4_BS1);
    qacc* rows[3] = {&qr, &qg, &qb};
    for(qacc* q : rows) q->mulas(-32768, -32768), q->mulas(-16384, 3);
    *r = op_max(qr.srcmb(16), 0);
    *g = op_max(qg.srcmb(16), 0);
    *b = op_max(qb.srcmb(16), 0);
}
} // namespace

// ---------------------------------------------------------------- the stand ins

void tone_fx_cbrt(const q15* x, int n, q15* y)
{
    for(int i = 0; i < n; i++) y[i] = cbrt_lane(x[i]);
}

void tone_fx_lin_to_oklab(const q15* r, const q15* g, const q15* b, int n, q15* L, q15* A, q15* B)
{
    for(int i = 0; i < n; i++)
    {
        int16_t l, a, bb;
        lin_to_oklab_lane(r[i], g[i], b[i], &l, &a, &bb);
        L[i] = l, A[i] = a, B[i] = bb;
    }
}

void tone_fx_oklab_to_lin(const q15* L, const q15* A, const q15* B, int n, q15* r, q15* g, q15* b)
{
    for(int i = 0; i < n; i++)
    {
        int16_t rr, gg, bb;
        oklab_to_lin_lane(L[i], A[i], B[i], &rr, &gg, &bb);
        r[i] = rr, g[i] = gg, b[i] = bb;
    }
}

void tone_fx_lin_to_oklab_1(const q15 rgb[8], q15 lab[8])
{
    int16_t l, a, b;
    lin_to_oklab_lane(rgb[0], rgb[1], rgb[2], &l, &a, &b);
    lab[0] = l, lab[1] = a, lab[2] = b;
}
