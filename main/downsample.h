// Scales a source image down to a smaller destination image by averaging the source pixels that map
// to each destination pixel, weighting each pixel's color by its alpha.
// It takes the source one row at a time, so the whole source need never be held at once.
// Call init once, then push each source row, then finish.
// push expects the source rows in order of increasing y.
// finish writes the last destination row and leaves out->alpha empty when every destination pixel
// came out fully opaque.
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"

struct downsampler
{
    int                   sw = 0, sh = 0, dw = 0, dh = 0, cur = -1;
    bool                  transparent = false;
    gimage*               out = nullptr;
    std::vector<uint32_t> sca, sa, cnt;
    std::vector<int>      xmap;

    void init(int sw_, int sh_, int dw_, int dh_, gimage* o)
    {
        sw  = sw_;
        sh  = sh_;
        dw  = dw_;
        dh  = dh_;
        out = o;
        o->w = dw;
        o->h = dh;
        o->px.assign(static_cast<size_t>(dw) * dh * 3, 255);
        o->alpha.assign(static_cast<size_t>(dw) * dh, 0);
        sca.assign(static_cast<size_t>(dw) * 3, 0);
        sa.assign(dw, 0);
        cnt.assign(dw, 0);
        xmap.resize(sw);
        for(int x = 0; x < sw; x++) xmap[x] = static_cast<int>(static_cast<long>(x) * dw / sw);
    }
    void push(int y, const uint8_t* c, const uint8_t* a)
    {
        int ty = static_cast<int>(static_cast<long>(y) * dh / sh);
        if(ty != cur)
        {
            flush();
            cur = ty;
        }
        for(int x = 0; x < sw; x++)
        {
            uint32_t al = a ? a[x] : 255;
            int      tx = xmap[x];
            for(int k = 0; k < 3; k++) sca[3 * tx + k] += c[3 * x + k] * al;
            sa[tx] += al;
            cnt[tx]++;
        }
    }
    void flush()
    {
        if(cur < 0 || cur >= dh) return;
        for(int tx = 0; tx < dw; tx++)
        {
            size_t o = static_cast<size_t>(cur) * dw + tx;
            if(cnt[tx] && sa[tx])
                for(int k = 0; k < 3; k++) out->px[3 * o + k] = static_cast<uint8_t>(sca[3 * tx + k] / sa[tx]);
            int am = cnt[tx] ? static_cast<int>(sa[tx] / cnt[tx]) : 0;
            out->alpha[o] = static_cast<uint8_t>(am);
            if(am < 255) transparent = true;
            sca[3 * tx] = sca[3 * tx + 1] = sca[3 * tx + 2] = sa[tx] = cnt[tx] = 0;
        }
    }
    void finish()
    {
        flush();
        cur = -1;
        if(!transparent) std::vector<uint8_t>().swap(out->alpha);
    }
};
