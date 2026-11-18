// A host model of the ESP32-S3 vector instructions the sRGB and bracket kernels use, written from
// the operation listings of the ESP32-S3 Technical Reference Manual, chapter 1 (Processor
// Instruction Extensions). srgb_test.cpp and bracket_test.cpp run each kernel's instruction sequence
// on it to check the shuffles and the lane arithmetic on the host. It checks the sequences against
// the manual's semantics, not the hardware.
#pragma once

#include <cstdint>
#include <cstring>

struct qreg
{
    uint8_t b[16];

    int16_t lane(int i) const
    {
        return static_cast<int16_t>(b[2 * i] | b[2 * i + 1] << 8);
    }
    void set(int i, int16_t v)
    {
        b[2 * i]     = static_cast<uint8_t>(v);
        b[2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    }
};

struct pie
{
    qreg q[8];
    int  sar_byte = 0;
    int  sar      = 0;
    // The loads of ld_usar and vld whose block starts in [win_lo, win_hi): the lowest and the highest
    // block start, for a check of the bytes a kernel reads around its input.
    uintptr_t win_lo = 0, win_hi = 0, rd_lo = UINTPTR_MAX, rd_hi = 0;

    static const uint8_t* align(const void* p)
    {
        return reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(15));
    }

    void note(const void* p)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(align(p));
        if(a < win_lo || a >= win_hi) return;
        if(a < rd_lo) rd_lo = a;
        if(a > rd_hi) rd_hi = a;
    }

    // ee.ld.128.usar.ip qu, as, imm
    void ld_usar(int u, const uint8_t*& as, int imm)
    {
        note(as);
        memcpy(q[u].b, align(as), 16);
        sar_byte = static_cast<int>(reinterpret_cast<uintptr_t>(as) & 15);
        as += imm;
    }
    // ee.vld.128.ip qu, as, imm
    template <typename T> void vld(int u, T*& as, int imm)
    {
        note(as);
        memcpy(q[u].b, align(as), 16);
        as = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(as) + imm);
    }
    // ee.vst.128.ip qv, as, imm
    template <typename T> void vst(int v, T*& as, int imm)
    {
        memcpy(const_cast<uint8_t*>(align(as)), q[v].b, 16);
        as = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(as) + imm);
    }
    // ee.vldbc.16.ip qu, as, imm
    void vldbc16(int u, const int16_t*& as, int imm)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(as) & ~static_cast<uintptr_t>(1));
        for(int i = 0; i < 8; i++) q[u].b[2 * i] = p[0], q[u].b[2 * i + 1] = p[1];
        as = reinterpret_cast<const int16_t*>(reinterpret_cast<uintptr_t>(as) + imm);
    }
    // ee.src.q qa, qs0, qs1: {qs1, qs0} >> 8 SAR_BYTE, low 128 bits.
    void src_q(int a, int s0, int s1)
    {
        uint8_t cat[32];
        memcpy(cat, q[s0].b, 16);
        memcpy(cat + 16, q[s1].b, 16);
        memcpy(q[a].b, cat + sar_byte, 16);
    }
    // ee.srci.2q qs1, qs0, imm: {qs1, qs0} >> 8 (imm + 1), zero filled.
    void srci_2q(int s1, int s0, int imm)
    {
        uint8_t cat[64] = {0};
        memcpy(cat, q[s0].b, 16);
        memcpy(cat + 16, q[s1].b, 16);
        memcpy(q[s0].b, cat + imm + 1, 16);
        memcpy(q[s1].b, cat + 16 + imm + 1, 16);
    }
    // ee.vzip.8 qs0, qs1: qs0 = low halves interleaved, qs1 = high halves interleaved.
    void vzip8(int s0, int s1)
    {
        qreg a = q[s0], c = q[s1];
        for(int i = 0; i < 8; i++)
        {
            q[s0].b[2 * i] = a.b[i], q[s0].b[2 * i + 1] = c.b[i];
            q[s1].b[2 * i] = a.b[8 + i], q[s1].b[2 * i + 1] = c.b[8 + i];
        }
    }
    void xorq(int a, int x, int y)
    {
        for(int i = 0; i < 16; i++) q[a].b[i] = q[x].b[i] ^ q[y].b[i];
    }
    void orq(int a, int x, int y)
    {
        for(int i = 0; i < 16; i++) q[a].b[i] = q[x].b[i] | q[y].b[i];
    }
    void andq(int a, int x, int y)
    {
        for(int i = 0; i < 16; i++) q[a].b[i] = q[x].b[i] & q[y].b[i];
    }
    void zero(int a)
    {
        memset(q[a].b, 0, 16);
    }
    // ee.vcmp.lt.s16 qa, qx, qy: 0xffff where qx < qy, else 0.
    void vcmplt16(int a, int x, int y)
    {
        qreg r;
        for(int i = 0; i < 8; i++) r.set(i, q[x].lane(i) < q[y].lane(i) ? -1 : 0);
        q[a] = r;
    }
    void vmin16(int a, int x, int y)
    {
        qreg r;
        for(int i = 0; i < 8; i++) r.set(i, q[x].lane(i) < q[y].lane(i) ? q[x].lane(i) : q[y].lane(i));
        q[a] = r;
    }
    void vmax16(int a, int x, int y)
    {
        qreg r;
        for(int i = 0; i < 8; i++) r.set(i, q[x].lane(i) > q[y].lane(i) ? q[x].lane(i) : q[y].lane(i));
        q[a] = r;
    }
    // ee.vmul.s16 qz, qx, qy: (qx * qy) >> SAR arithmetic, low 16 bits.
    void vmul16(int z, int x, int y)
    {
        qreg r;
        for(int i = 0; i < 8; i++)
            r.set(i, static_cast<int16_t>(
                         static_cast<uint16_t>((static_cast<int32_t>(q[x].lane(i)) * q[y].lane(i)) >> (sar & 63))));
        q[z] = r;
    }
    // ee.vadds.s16 qa, qx, qy: saturating.
    void vadds16(int a, int x, int y)
    {
        qreg r;
        for(int i = 0; i < 8; i++)
        {
            const int32_t s = static_cast<int32_t>(q[x].lane(i)) + q[y].lane(i);
            r.set(i, static_cast<int16_t>(s > 32767 ? 32767 : s < -32768 ? -32768 : s));
        }
        q[a] = r;
    }
    // ee.vunzip.8 qs0, qs1: qs0 = even bytes of qs0 then of qs1, qs1 = odd bytes of qs0 then of qs1.
    void vunzip8(int s0, int s1)
    {
        qreg a = q[s0], c = q[s1];
        for(int i = 0; i < 8; i++)
        {
            q[s0].b[i] = a.b[2 * i], q[s0].b[8 + i] = c.b[2 * i];
            q[s1].b[i] = a.b[2 * i + 1], q[s1].b[8 + i] = c.b[2 * i + 1];
        }
    }
};
