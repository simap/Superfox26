#include "tabs.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "esp_log.h"

#include "downsample.h"
#include "image.h"
#include "tones.h"
#include "image_legacy.h"

static const char* TAG      = "tabs";
static const char* TAB_DIR  = CFG_BASE "/tabs";
// TAB1: magic, title length, icon length, title, icon. TAB2 adds a flags byte after the magic.
static const char  MAGIC1[4] = {'T', 'A', 'B', '1'};
static const char  MAGIC2[4] = {'T', 'A', 'B', '2'};
static const uint8_t FLAG_PLACEHOLDER = 1;
static const int   ICON     = 32;

namespace
{
uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for(unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string path_for(const std::string& url)
{
    char name[24];
    snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)fnv1a(url));
    return TAB_DIR + std::string(name);
}

uint32_t le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
void     put16(std::string& s, uint32_t v) { s.push_back(static_cast<char>(v & 0xff)); s.push_back(static_cast<char>((v >> 8) & 0xff)); }
void     put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }

// Largest size with the aspect of w x h that fits in ICON x ICON.
void fit_icon(int w, int h, int& tw, int& th)
{
    int s = std::max(w, h);
    tw    = std::max(1, static_cast<int>(static_cast<long long>(w) * ICON / s));
    th    = std::max(1, static_cast<int>(static_cast<long long>(h) * ICON / s));
}

// 16x16 32-bit BGRA bottom-up BMP from a decoded image: fitted with its aspect, centred, the margin
// transparent, nearest neighbour when the image is smaller than the fitted size.
std::string bmp_from_gimage(const gimage& img)
{
    if(img.w <= 0 || img.h <= 0 || img.px.empty()) return "";
    int tw = 0, th = 0;
    fit_icon(img.w, img.h, tw, th);
    int ox = (ICON - tw) / 2, oy = (ICON - th) / 2;
    std::string out;
    out.reserve(14 + 40 + ICON * ICON * 4);
    out += "BM";
    put32(out, 14 + 40 + ICON * ICON * 4);
    put32(out, 0);
    put32(out, 14 + 40);
    put32(out, 40);
    put32(out, ICON);
    put32(out, ICON);
    put16(out, 1);
    put16(out, 32);
    put32(out, 0); // BI_RGB, the fourth byte is alpha
    put32(out, ICON * ICON * 4);
    put32(out, 2835);
    put32(out, 2835);
    put32(out, 0);
    put32(out, 0);
    for(int y = ICON - 1; y >= 0; y--)
        for(int x = 0; x < ICON; x++)
        {
            if(x < ox || x >= ox + tw || y < oy || y >= oy + th)
            {
                out.append("\xff\xff\xff", 3);
                out.push_back(0);
                continue;
            }
            int    sx = (x - ox) * img.w / tw, sy = (y - oy) * img.h / th;
            size_t o  = static_cast<size_t>(sy) * img.w + sx;
            out.push_back(static_cast<char>(img.px[3 * o + 2]));
            out.push_back(static_cast<char>(img.px[3 * o + 1]));
            out.push_back(static_cast<char>(img.px[3 * o]));
            out.push_back(static_cast<char>(img.alpha_at(o)));
        }
    return out;
}

const char* const KIND_NAME[] = {"none", "png", "jpeg", "gif", "webp", "bmp"};

// Decode any supported image file, sniffed from its bytes, to fit in ICON x ICON.
bool decode_fit(const std::string& file, gimage& img, std::string* fmt)
{
    int w = 0, h = 0;
    int kind = sniff_size(file, w, h);
    if(!kind || w <= 0 || h <= 0) return false;
    if(fmt)
    {
        char d[40];
        snprintf(d, sizeof(d), "%s %dx%d", KIND_NAME[kind], w, h);
        *fmt += d;
    }
    int dw = w, dh = h;
    if(std::max(w, h) > ICON) fit_icon(w, h, dw, dh);
    return decode_image(kind, file, img, dw, dh);
}

// BMP entry of an ICO: a BITMAPINFOHEADER without the file header, the height counting the color
// (XOR) rows and the 1-bit AND mask rows that follow them. The mask gives the transparency when the
// color rows carry no alpha. Decoded at natural size (at most 256x256), then box filtered.
bool decode_ico_bmp(const std::string& sub, gimage& img, std::string* fmt)
{
    auto b = reinterpret_cast<const unsigned char*>(sub.data());
    if(sub.size() < 40) return false;
    uint32_t hdr  = le32(b);
    int      w    = static_cast<int>(static_cast<int32_t>(le32(b + 4)));
    int      hh   = static_cast<int>(static_cast<int32_t>(le32(b + 8))) / 2;
    int      bpp  = static_cast<int>(le16(b + 14));
    uint32_t comp = le32(b + 16);
    uint32_t clr  = le32(b + 32);
    if(hdr < 40 || hdr > 124 || w <= 0 || w > 256 || hh <= 0 || hh > 256 || bpp <= 0 || bpp > 32) return false;
    if(fmt)
    {
        char d[40];
        snprintf(d, sizeof(d), "bmp %dx%d %d bpp", w, hh, bpp);
        *fmt += d;
    }
    uint32_t palette = bpp <= 8 ? (clr && clr < (1u << bpp) ? clr : (1u << bpp)) * 4 : 0;
    if(hdr == 40 && comp == 3) palette = 12; // BI_BITFIELDS masks
    std::string bmp = "BM";
    put32(bmp, 14 + static_cast<uint32_t>(sub.size()));
    put32(bmp, 0);
    put32(bmp, 14 + hdr + palette);
    bmp += sub;
    // the height field is at byte 8 of the info header, offset 14 in the file
    bmp[14 + 8]  = static_cast<char>(hh & 0xff);
    bmp[14 + 9]  = static_cast<char>((hh >> 8) & 0xff);
    bmp[14 + 10] = static_cast<char>((hh >> 16) & 0xff);
    bmp[14 + 11] = static_cast<char>((hh >> 24) & 0xff);
    gimage full;
    if(!decode_bmp(bmp, full, w, hh) || full.w != w || full.h != hh) return false;
    size_t xor_stride  = (static_cast<size_t>(w) * bpp + 31) / 32 * 4;
    size_t mask_stride = (static_cast<size_t>(w) + 31) / 32 * 4;
    size_t mask_off    = hdr + palette + xor_stride * hh;
    if(full.alpha.empty() && mask_off + mask_stride * hh <= sub.size())
    {
        // mask rows run bottom-up, a set bit is transparent
        full.alpha.assign(static_cast<size_t>(w) * hh, 255);
        for(int y = 0; y < hh; y++)
        {
            const unsigned char* m = b + mask_off + mask_stride * (hh - 1 - y);
            for(int x = 0; x < w; x++)
                if((m[x >> 3] >> (7 - (x & 7))) & 1) full.alpha[static_cast<size_t>(y) * w + x] = 0;
        }
    }
    int dw = w, dh = hh;
    if(std::max(w, hh) > ICON) fit_icon(w, hh, dw, dh);
    downsampler ds;
    ds.init(w, hh, dw, dh, &img);
    for(int y = 0; y < hh; y++)
        ds.push(y, &full.px[static_cast<size_t>(y) * w * 3],
                full.alpha.empty() ? nullptr : &full.alpha[static_cast<size_t>(y) * w]);
    ds.finish();
    return true;
}

bool is_ico(const std::string& file)
{
    auto u = reinterpret_cast<const unsigned char*>(file.data());
    return file.size() >= 6 && le16(u) == 0 && le16(u + 2) == 1 && le16(u + 4) > 0;
}

// ICO: a directory of images, each a PNG or a BMP entry. The entry nearest to ICON pixels and at least
// that wide wins, else the largest below. Among entries of one size the deepest color wins. Entries
// that end past the end of file are skipped, so a prefix of a large ICO still decodes.
bool decode_ico(const std::string& file, gimage& img, std::string* fmt)
{
    auto   u     = reinterpret_cast<const unsigned char*>(file.data());
    size_t n     = file.size();
    int    count = static_cast<int>(le16(u + 4));
    int    best  = -1;
    long   best_score = LONG_MAX;
    for(int i = 0; i < count && 6 + 16 * (static_cast<size_t>(i) + 1) <= n; i++)
    {
        const unsigned char* e  = u + 6 + 16 * i;
        uint32_t             sz = le32(e + 8), off = le32(e + 12);
        if(off > n || sz > n - off || sz < 16) continue;
        int  w     = e[0] ? e[0] : 256;
        int  bpp   = file.compare(off, 4, "\x89PNG") == 0 ? 32 : (sz >= 40 ? static_cast<int>(le16(u + off + 14)) : 0);
        long score = (w >= ICON ? w - ICON : (ICON - w) * 8 + 1000) * 64L + (32 - std::min(bpp, 32));
        if(score < best_score)
        {
            best_score = score;
            best       = i;
        }
    }
    if(best < 0) return false;
    const unsigned char* e   = u + 6 + 16 * best;
    std::string          sub = file.substr(le32(e + 12), le32(e + 8));
    if(fmt) *fmt = "ico, ";
    int w = 0, h = 0;
    if(sniff_size(sub, w, h) == IMG_PNG) return decode_fit(sub, img, fmt);
    return decode_ico_bmp(sub, img, fmt);
}

const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string& in)
{
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for(; i + 2 < in.size(); i += 3)
    {
        uint32_t v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                     static_cast<unsigned char>(in[i + 2]);
        out.push_back(b64[v >> 18]);
        out.push_back(b64[(v >> 12) & 63]);
        out.push_back(b64[(v >> 6) & 63]);
        out.push_back(b64[v & 63]);
    }
    if(i < in.size())
    {
        uint32_t v = static_cast<unsigned char>(in[i]) << 16;
        if(i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out.push_back(b64[v >> 18]);
        out.push_back(b64[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? b64[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// 5x7 glyphs for A to Z, then 0 to 9. One byte per row, top row first, bit 4 the leftmost column.
const uint8_t GLYPHS[36][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};
// The 7 glyph rows stretched to 12 icon rows: the middle and outer rows doubled, rows 2 and 4 single.
const uint8_t GLYPH_ROW[12] = {0, 0, 1, 1, 2, 3, 3, 4, 5, 5, 6, 6};
} // namespace

std::string html_escape(const std::string& s)
{
    std::string out;
    for(char c : s)
    {
        if(c == '&') out += "&amp;";
        else if(c == '<') out += "&lt;";
        else if(c == '>') out += "&gt;";
        else if(c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

bool tabs_load(const std::string& url, tab_info& out)
{
    out = tab_info();
    FILE* f = fopen(path_for(url).c_str(), "rb");
    if(!f) return false;
    char     magic[4];
    uint8_t  flags = 0;
    uint32_t tl = 0, bl = 0;
    bool     ok = fread(magic, 1, 4, f) == 4;
    bool     v2 = ok && memcmp(magic, MAGIC2, 4) == 0;
    ok          = ok && (v2 || memcmp(magic, MAGIC1, 4) == 0) && (!v2 || fread(&flags, 1, 1, f) == 1) &&
         fread(&tl, 4, 1, f) == 1 && fread(&bl, 4, 1, f) == 1 && tl < 512 && bl < 8192;
    if(ok)
    {
        out.title.resize(tl);
        ok = tl == 0 || fread(&out.title[0], 1, tl, f) == tl;
    }
    if(ok)
    {
        out.icon_bmp.resize(bl);
        if(bl && fread(&out.icon_bmp[0], 1, bl, f) != bl)
        {
            ESP_LOGW(TAG, "%s: icon cut off, dropped", path_for(url).c_str());
            out.icon_bmp.clear();
        } else
            out.placeholder = (flags & FLAG_PLACEHOLDER) != 0;
        // an icon stored at another size (an older build) is shown but fetched again
        if(out.icon_bmp.size() >= 22 &&
           le32(reinterpret_cast<const unsigned char*>(out.icon_bmp.data()) + 18) != static_cast<uint32_t>(ICON))
            out.placeholder = true;
    }
    fclose(f);
    if(!ok) out = tab_info();
    return ok;
}

bool tabs_save(const std::string& url, const tab_info& t)
{
    // no write when the file already holds this: the flash is left alone once every page is known
    tab_info cur;
    if(tabs_load(url, cur) && cur.title == t.title && cur.icon_bmp == t.icon_bmp && cur.placeholder == t.placeholder)
        return true;
    struct stat sb;
    if(stat(TAB_DIR, &sb) != 0 && mkdir(TAB_DIR, 0777) != 0 && errno != EEXIST)
    {
        ESP_LOGW(TAG, "cannot create %s: errno %d", TAB_DIR, errno);
        return false;
    }
    std::string path = path_for(url), tmp = path + ".tmp";
    FILE*       f    = fopen(tmp.c_str(), "wb");
    if(!f)
    {
        ESP_LOGW(TAG, "cannot write %s: errno %d", tmp.c_str(), errno);
        return false;
    }
    uint8_t  flags = t.placeholder ? FLAG_PLACEHOLDER : 0;
    uint32_t tl = t.title.size(), bl = t.icon_bmp.size();
    bool     ok = fwrite(MAGIC2, 1, 4, f) == 4 && fwrite(&flags, 1, 1, f) == 1 && fwrite(&tl, 4, 1, f) == 1 &&
              fwrite(&bl, 4, 1, f) == 1 && fwrite(t.title.data(), 1, tl, f) == tl &&
              fwrite(t.icon_bmp.data(), 1, bl, f) == bl;
    if(fclose(f) != 0) ok = false;
    // LittleFS rename replaces the target in one step
    if(ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if(!ok)
    {
        ESP_LOGW(TAG, "cannot write %s: errno %d", path.c_str(), errno);
        remove(tmp.c_str());
    }
    return ok;
}

std::string tabs_icon_bmp(const std::string& file, bool partial, std::string* format)
{
    gimage img;
    if(format) format->clear();
    bool ico = is_ico(file);
    if(partial && !ico) return "";
    bool ok = ico ? decode_ico(file, img, format) : decode_fit(file, img, format);
    return ok ? bmp_from_gimage(img) : "";
}

// logo/panel_32.png, at the left end of the bar (EMBED_FILES in CMakeLists.txt).
extern const uint8_t logo_32_start[] asm("_binary_panel_32_png_start");
extern const uint8_t logo_32_end[] asm("_binary_panel_32_png_end");

std::string tabs_png_url(const uint8_t* begin, const uint8_t* end)
{
    return "data:image/png;base64," + b64_encode(std::string(begin, end));
}

std::string tabs_placeholder_bmp(const std::string& host)
{
    int glyph = -1;
    for(unsigned char c : host)
    {
        if(c >= 'a' && c <= 'z') glyph = c - 'a';
        else if(c >= 'A' && c <= 'Z') glyph = c - 'A';
        else if(c >= '0' && c <= '9') glyph = 26 + c - '0';
        else continue;
        break;
    }
    gimage img;
    img.w = img.h = ICON;
    img.px.assign(ICON * ICON * 3, 255);
    // On a 16 px grid scaled by s: a 10 x 12 glyph cell at columns 3 to 12, rows 2 to 13, inside an
    // s px border
    const int s = ICON / 16;
    for(int y = 0; y < ICON; y++)
        for(int x = 0; x < ICON; x++)
        {
            bool ink = x < s || y < s || x >= ICON - s || y >= ICON - s;
            int  gx = x / s, gy = y / s;
            if(!ink && glyph >= 0 && gx >= 3 && gx < 13 && gy >= 2 && gy < 14)
                ink = (GLYPHS[glyph][GLYPH_ROW[gy - 2]] >> (4 - (gx - 3) / 2)) & 1;
            if(ink) memset(&img.px[3 * (static_cast<size_t>(y) * ICON + x)], 0, 3);
        }
    return bmp_from_gimage(img);
}

std::string tabs_host(const std::string& url)
{
    auto p = url.find("://");
    if(p == std::string::npos) return url;
    auto q    = url.find('/', p + 3);
    auto host = url.substr(p + 3, q == std::string::npos ? std::string::npos : q - p - 3);
    if(host.compare(0, 4, "www.") == 0) host = host.substr(4);
    return host;
}

std::string tabs_html(const std::vector<page_entry>& pages, size_t current, const std::vector<tab_info>& infos,
                      int tab_h)
{
    // The tabs start at row 0, a 1 px rule under them. Two tones: the row behind the tabs is a mid
    // gray, the other tabs a light gray, the current tab paper like the page below it. The fills are
    // grays that map onto a neutral tone entry exactly, so they render solid. The border is two 1 px
    // lines: black outside, and inside a raised bevel, white on the top and left, the row gray on the
    // right. The logo comes first, on the row gray, centred in the rows above the rule.
    int  inner = tab_h - 1;
    int  row = tones_solid_gray(174), fill = tones_solid_gray(200);
    char css[900];
    snprintf(css, sizeof(css),
             "body{margin:0;padding:0;background:#%02x%02x%02x}"
             ".bar{display:flex;height:%dpx;padding:0 6px;border-bottom:1px solid #000;box-sizing:border-box}"
             ".logo{flex:none;width:%dpx;height:%dpx;margin:%dpx 6px 0 0}"
             ".tab{flex:1 1 0;min-width:0;height:%dpx;margin-right:4px;border:1px solid #000;border-bottom:none;"
             "border-radius:9px 9px 0 0;box-sizing:border-box;background:#%02x%02x%02x}"
             ".in{height:%dpx;border:1px solid #fff;border-right-color:#%02x%02x%02x;border-bottom:none;"
             "border-radius:8px 8px 0 0;padding:0 6px;box-sizing:border-box;font-family:Arial;font-weight:bold;"
             "font-size:17px;line-height:%dpx;white-space:nowrap;overflow:hidden;color:#000}"
             ".cur{background:#fff}"
             ".in img{width:%dpx;height:%dpx;vertical-align:middle;margin-right:6px;position:relative;top:-1px}",
             row, row, row, tab_h, ICON, ICON, (inner - ICON) / 2, inner, fill, fill, fill, inner - 1, row, row, row,
             inner - 3, ICON, ICON);
    std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><style>";
    html += css;
    html += "</style></head><body><div class=\"bar\"><img class=\"logo\" src=\"";
    html += tabs_png_url(logo_32_start, logo_32_end);
    html += "\">";
    for(size_t i = 0; i < pages.size(); i++)
    {
        const tab_info* t = i < infos.size() ? &infos[i] : nullptr;
        html += i == current ? "<div class=\"tab cur\"><div class=\"in\">" : "<div class=\"tab\"><div class=\"in\">";
        if(t && !t->icon_bmp.empty()) html += "<img src=\"data:image/bmp;base64," + b64_encode(t->icon_bmp) + "\">";
        html += html_escape(t && !t->title.empty() ? t->title : tabs_host(pages[i].url));
        html += "</div></div>";
    }
    html += "</div></body></html>";
    return html;
}
