#include "image_gif_bmp.h"
#include "downsample.h"
#include "util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include "esp_log.h"

static const char* TAG = "image";

namespace
{
const long MAX_PIXELS = 1L << 24;

bool too_big(int w, int h)
{
    if(static_cast<long long>(w) * h <= MAX_PIXELS) return false;
    ESP_LOGW(TAG, "%dx%d is over the %ld pixel limit", w, h, MAX_PIXELS);
    return true;
}

struct lzw
{
    const uint8_t* p = nullptr;
    size_t         n = 0, off = 0;
    int            blk = 0;
    uint32_t       bits = 0;
    int            nbits = 0, min_size = 0, size = 0, clear = 0, eoi = 0, avail = 0, old = -1, sp = 0;
    uint8_t        first = 0;
    bool           end   = false;
    uint16_t       prefix[4096];
    uint8_t        suffix[4096];
    uint8_t        stack[4097];

    void init(const uint8_t* d, size_t len, size_t o, int ms)
    {
        p        = d;
        n        = len;
        off      = o;
        min_size = ms;
        clear    = 1 << ms;
        eoi      = clear + 1;
        reset();
    }
    void reset()
    {
        size  = min_size + 1;
        avail = eoi + 1;
        old   = -1;
    }
    int byte()
    {
        if(!blk && (off >= n || !(blk = p[off++]))) return -1;
        if(off >= n) return -1;
        blk--;
        return p[off++];
    }
    int code()
    {
        while(nbits < size)
        {
            int b = byte();
            if(b < 0) return -1;
            bits |= static_cast<uint32_t>(b) << nbits;
            nbits += 8;
        }
        int c = static_cast<int>(bits & ((1u << size) - 1));
        bits >>= size;
        nbits -= size;
        return c;
    }
    int pixel()
    {
        if(sp) return stack[--sp];
        if(end) return -1;
        for(;;)
        {
            int c = code();
            if(c == clear)
            {
                reset();
                continue;
            }
            if(c < 0 || c == eoi || c > avail || (old < 0 && c > clear))
            {
                end = true;
                return -1;
            }
            if(old < 0)
            {
                old = first = static_cast<uint8_t>(c);
                return c;
            }
            int in = c;
            if(c == avail)
            {
                stack[sp++] = first;
                c           = old;
            }
            while(c > eoi)
            {
                stack[sp++] = suffix[c];
                c           = prefix[c];
            }
            first = static_cast<uint8_t>(c);
            if(avail < 4096)
            {
                prefix[avail] = static_cast<uint16_t>(old);
                suffix[avail] = first;
                if(++avail == (1 << size) && size < 12) size++;
            }
            old = in;
            return c;
        }
    }
};

bool gif_first_frame(const std::string& data, gimage& img, int dw, int dh)
{
    auto   u = reinterpret_cast<const uint8_t*>(data.data());
    size_t n = data.size();
    if(n < 13 || memcmp(u, "GIF8", 4)) return false;
    int sw = static_cast<int>(le16(u + 6)), sh = static_cast<int>(le16(u + 8));
    dw = std::min(dw, sw);
    dh = std::min(dh, sh);
    if(dw <= 0 || dh <= 0 || too_big(sw, sh)) return false;
    size_t         off  = 13;
    const uint8_t* gct  = nullptr;
    int            gcnt = 0;
    if(u[10] & 0x80)
    {
        gcnt = 2 << (u[10] & 7);
        if(off + 3 * gcnt > n) return false;
        gct = u + off;
        off += 3 * gcnt;
    }
    int trans = -1;
    for(;;)
    {
        if(off >= n) return false;
        int rt = u[off++];
        if(rt == 0x21)
        {
            if(off >= n) return false;
            int  label = u[off++];
            bool head  = true;
            for(;;)
            {
                if(off >= n) return false;
                size_t len = u[off++];
                if(!len) break;
                if(off + len > n) return false;
                if(label == 0xF9 && head && len >= 4) trans = (u[off] & 1) ? u[off + 3] : -1;
                head = false;
                off += len;
            }
            continue;
        }
        if(rt != 0x2C) return false;
        if(off + 9 > n) return false;
        int fl = static_cast<int>(le16(u + off)), ft = static_cast<int>(le16(u + off + 2));
        int fw = static_cast<int>(le16(u + off + 4)), fh = static_cast<int>(le16(u + off + 6));
        int flags = u[off + 8];
        off += 9;
        const uint8_t* ct  = gct;
        int            cnt = gcnt;
        if(flags & 0x80)
        {
            cnt = 2 << (flags & 7);
            if(off + 3 * cnt > n) return false;
            ct = u + off;
            off += 3 * cnt;
        }
        if(off >= n || fw <= 0 || fh <= 0) return false;
        int ms = u[off++];
        if(ms < 2 || ms > 8)
        {
            ESP_LOGW(TAG, "gif: LZW minimum code size %d", ms);
            return false;
        }
        std::unique_ptr<lzw> z(new(std::nothrow) lzw);
        if(!z) return false;
        z->init(u, n, off, ms);
        int                  vw = std::max(0, std::min(fw, sw - fl)), vh = std::max(0, std::min(fh, sh - ft));
        std::vector<uint8_t> line(fw);
        long                 got = 0;
        bool                 tried = false, stopped = false;
        auto                 read_row = [&](uint8_t* dst) {
            tried = true;
            int k = 0;
            for(int c; k < fw && (c = z->pixel()) >= 0; k++) dst[k] = static_cast<uint8_t>(c);
            got += k;
            if(k < fw) stopped = true;
            return std::min(k, vw);
        };
        std::unique_ptr<uint8_t, decltype(&free)> raster(nullptr, &free);
        std::vector<uint16_t>                     rowlen;
        if((flags & 0x40) && vw && vh)
        {
            raster.reset(static_cast<uint8_t*>(malloc(static_cast<size_t>(vw) * vh)));
            if(!raster) return false;
            rowlen.assign(vh, 0);
            static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            for(int pass = 0; pass < 4 && !stopped; pass++)
                for(int y = start[pass]; y < fh && !stopped; y += step[pass])
                {
                    int k = read_row(line.data());
                    if(y < vh)
                    {
                        memcpy(raster.get() + static_cast<size_t>(y) * vw, line.data(), k);
                        rowlen[y] = static_cast<uint16_t>(k);
                    }
                }
        }
        downsampler ds;
        ds.init(sw, sh, dw, dh, &img);
        std::vector<uint8_t> rgb(static_cast<size_t>(sw) * 3), al(sw);
        for(int y = 0; y < sh; y++)
        {
            std::fill(rgb.begin(), rgb.end(), 255);
            std::fill(al.begin(), al.end(), 0);
            int fy = y - ft;
            if(fy >= 0 && fy < vh)
            {
                const uint8_t* src = nullptr;
                int            len = 0;
                if(raster)
                {
                    src = raster.get() + static_cast<size_t>(fy) * vw;
                    len = rowlen[fy];
                } else if(!stopped)
                {
                    src = line.data();
                    len = read_row(line.data());
                }
                for(int fx = 0; fx < len; fx++)
                {
                    int idx = src[fx], x = fl + fx;
                    if(idx == trans || idx >= cnt) continue;
                    memcpy(&rgb[3 * x], ct + 3 * idx, 3);
                    al[x] = 255;
                }
            }
            ds.push(y, rgb.data(), al.data());
        }
        ds.finish();
        if(tried && !got)
        {
            ESP_LOGW(TAG, "gif: no pixel data");
            return false;
        }
        if(stopped) ESP_LOGW(TAG, "gif: frame data ends early, %ld of %ld pixels", got, static_cast<long>(fw) * fh);
        return true;
    }
}

struct chan
{
    uint32_t mask = 0;
    int      shift = 0, bits = 0;
    void     set(uint32_t m)
    {
        mask  = m;
        shift = m ? __builtin_ctz(m) : 0;
        bits  = __builtin_popcount(m);
    }
    uint8_t get(uint32_t v) const
    {
        if(!mask) return 0;
        uint32_t x = (v & mask) >> shift;
        if(bits >= 8) return static_cast<uint8_t>(x >> (bits - 8));
        uint32_t m = (1u << bits) - 1;
        return static_cast<uint8_t>((x * 255 + m / 2) / m);
    }
};

struct bmp_hdr
{
    int  hdr = 0, w = 0, h = 0, bpp = 0, comp = 0;
    bool topdown = false;
};

bool bmp_header(const uint8_t* u, size_t n, bmp_hdr& b)
{
    if(n < 26 || u[0] != 'B' || u[1] != 'M') return false;
    b.hdr = static_cast<int>(le32(u + 14));
    long long h;
    if(b.hdr == 12)
    {
        b.w   = static_cast<int>(le16(u + 18));
        h     = le16(u + 20);
        b.bpp = static_cast<int>(le16(u + 24));
    } else if(b.hdr == 40 || b.hdr == 52 || b.hdr == 56 || b.hdr == 64 || b.hdr == 108 || b.hdr == 124)
    {
        if(n < 34) return false;
        b.w    = static_cast<int>(static_cast<int32_t>(le32(u + 18)));
        h      = static_cast<int32_t>(le32(u + 22));
        b.bpp  = static_cast<int>(le16(u + 28));
        b.comp = static_cast<int>(le32(u + 30));
    } else
        return false;
    b.topdown = h < 0;
    if(h < 0) h = -h;
    if(b.w <= 0 || b.w > 65535 || h <= 0 || h > 65535) return false;
    b.h = static_cast<int>(h);
    return true;
}

bool bmp_decode(const std::string& data, gimage& img, int dw, int dh)
{
    auto    u = reinterpret_cast<const uint8_t*>(data.data());
    size_t  n = data.size();
    bmp_hdr b;
    if(!bmp_header(u, n, b) || n < 14 + static_cast<size_t>(b.hdr)) return false;
    int sw = b.w, sh = b.h, bpp = b.bpp, comp = b.comp;
    dw = std::min(dw, sw);
    dh = std::min(dh, sh);
    if(dw <= 0 || dh <= 0 || too_big(sw, sh)) return false;
    bool bitfields = (comp == 3 || comp == 6) && b.hdr != 64;
    if(!(comp == 0 || bitfields))
    {
        ESP_LOGW(TAG, "bmp: compression %d not supported", comp);
        return false;
    }
    if(bitfields ? !(bpp == 16 || bpp == 32) : !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32))
    {
        ESP_LOGW(TAG, "bmp: %d bits per pixel with compression %d not supported", bpp, comp);
        return false;
    }
    size_t pal = 14 + static_cast<size_t>(b.hdr);
    chan   r, g, bl, a;
    if(bitfields)
    {
        size_t nm = b.hdr >= 56 ? 4 : b.hdr == 52 ? 3 : comp == 6 ? 4 : 3;
        if(b.hdr == 40) pal += 4 * nm;
        if(54 + 4 * nm > n) return false;
        r.set(le32(u + 54));
        g.set(le32(u + 58));
        bl.set(le32(u + 62));
        if(nm == 4) a.set(le32(u + 66));
    } else if(bpp == 16)
    {
        r.set(0x7C00);
        g.set(0x03E0);
        bl.set(0x001F);
    } else if(bpp >= 24)
    {
        r.set(0xFF0000);
        g.set(0x00FF00);
        bl.set(0x0000FF);
        if(bpp == 32) a.set(0xFF000000);
    }
    int psz = b.hdr == 12 ? 3 : 4, pcnt = 0;
    if(bpp <= 8)
    {
        uint32_t used = b.hdr == 12 ? 0 : le32(u + 46);
        pcnt          = 1 << bpp;
        if(used && used < static_cast<uint32_t>(pcnt)) pcnt = static_cast<int>(used);
        if(pal > n) return false;
        pcnt = std::min(pcnt, static_cast<int>((n - pal) / psz));
    }
    size_t px     = le32(u + 10);
    size_t stride = (static_cast<size_t>(sw) * bpp + 31) / 32 * 4;
    size_t rowb   = (static_cast<size_t>(sw) * bpp + 7) / 8;
    int    bytes  = bpp / 8;
    auto   row_at = [&](int y) -> const uint8_t* {
        uint64_t o = px + static_cast<uint64_t>(b.topdown ? y : sh - 1 - y) * stride;
        return px < n && o + rowb <= n ? u + o : nullptr;
    };
    auto value = [&](const uint8_t* s, int x) {
        const uint8_t* q = s + static_cast<size_t>(x) * bytes;
        uint32_t       v = 0;
        for(int k = bytes - 1; k >= 0; k--) v = (v << 8) | q[k];
        return v;
    };
    bool alpha = false;
    if(a.mask)
        for(int y = 0; y < sh && !alpha; y++)
            if(const uint8_t* s = row_at(y))
                for(int x = 0; x < sw && !alpha; x++) alpha = a.get(value(s, x)) != 0;
    downsampler ds;
    ds.init(sw, sh, dw, dh, &img);
    std::vector<uint8_t> rgb(static_cast<size_t>(sw) * 3), al(sw);
    int                  rows = 0;
    for(int y = 0; y < sh; y++)
    {
        const uint8_t* s = row_at(y);
        if(!s)
        {
            std::fill(rgb.begin(), rgb.end(), 255);
            std::fill(al.begin(), al.end(), 0);
            ds.push(y, rgb.data(), al.data());
            continue;
        }
        rows++;
        for(int x = 0; x < sw; x++)
        {
            uint8_t* d = &rgb[3 * x];
            if(bpp <= 8)
            {
                int i = (s[(static_cast<size_t>(x) * bpp) >> 3] >> (8 - bpp - (x * bpp & 7))) & ((1 << bpp) - 1);
                if(i < pcnt)
                {
                    const uint8_t* e = u + pal + static_cast<size_t>(i) * psz;
                    d[0]             = e[2];
                    d[1]             = e[1];
                    d[2]             = e[0];
                } else
                    d[0] = d[1] = d[2] = 0;
            } else
            {
                uint32_t v = value(s, x);
                d[0]       = r.get(v);
                d[1]       = g.get(v);
                d[2]       = bl.get(v);
                if(alpha) al[x] = a.get(v);
            }
        }
        ds.push(y, rgb.data(), alpha ? al.data() : nullptr);
    }
    ds.finish();
    if(!rows)
    {
        ESP_LOGW(TAG, "bmp: no pixel rows in the file");
        return false;
    }
    if(rows < sh) ESP_LOGW(TAG, "bmp: file ends early, %d of %d rows", rows, sh);
    return true;
}
}

bool sniff_bmp(const std::string& data, int& w, int& h)
{
    bmp_hdr b;
    if(!bmp_header(reinterpret_cast<const uint8_t*>(data.data()), data.size(), b)) return false;
    w = b.w;
    h = b.h;
    return true;
}

bool decode_gif(const std::string& data, gimage& img, int dw, int dh)
{
    return gif_first_frame(data, img, dw, dh);
}

bool decode_bmp(const std::string& data, gimage& img, int dw, int dh)
{
    return bmp_decode(data, img, dw, dh);
}
