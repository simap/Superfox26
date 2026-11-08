// Box filter from a source image, pushed one row at a time top to bottom, to a smaller gimage.
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"

// Box filter from sw x sh source rows (pushed top to bottom) to dw x dh, dw <= sw and dh <= sh.
// Source rows are RGB, 3 bytes per pixel, with an optional alpha row. Color is alpha-weighted; the
// mask bit is the mean alpha after an 8x8 ordered dither. finish() drops the mask when every pixel
// was opaque.
struct downsampler
{
    int                   sw = 0, sh = 0, dw = 0, dh = 0, cur = -1;
    bool                  transparent = false;
    gimage*               out = nullptr;
    std::vector<uint32_t> sca, sa, cnt; // sca: 3 per target pixel
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
        o->mask.assign((static_cast<size_t>(dw) * dh + 7) / 8, 0);
        sca.assign(static_cast<size_t>(dw) * 3, 0);
        sa.assign(dw, 0);
        cnt.assign(dw, 0);
        xmap.resize(sw);
        for(int x = 0; x < sw; x++) xmap[x] = static_cast<int>(static_cast<long>(x) * dw / sw);
    }
    // c: sw RGB pixels, a: sw alpha bytes or nullptr for opaque
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
            // mask bit from the mean alpha, ordered-dithered so partial alpha keeps its coverage
            int am = cnt[tx] ? static_cast<int>(sa[tx] / cnt[tx]) : 0;
            int t  = (2 * bayer8[cur & 7][tx & 7] + 1) * 255 / 128;
            if(am >= 255 || (am > 0 && am >= t)) out->mask[o >> 3] |= static_cast<uint8_t>(0x80 >> (o & 7));
            if(am < 255) transparent = true;
            sca[3 * tx] = sca[3 * tx + 1] = sca[3 * tx + 2] = sa[tx] = cnt[tx] = 0;
        }
    }
    void finish()
    {
        flush();
        cur = -1;
        if(!transparent) std::vector<uint8_t>().swap(out->mask);
    }
};
