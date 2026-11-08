#include "tabs.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "esp_log.h"

#include "image.h"
#include "image_legacy.h"

static const char* TAG     = "tabs";
static const char* TAB_DIR = CFG_BASE "/tabs";
static const char  MAGIC[4] = {'T', 'A', 'B', '1'};
static const int   ICON    = 16;

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
    char name[80];
    snprintf(name, sizeof(name), "%s/%016llx.bin", TAB_DIR, (unsigned long long)fnv1a(url));
    return name;
}

uint32_t le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
void     put16(std::string& s, uint32_t v) { s.push_back(static_cast<char>(v & 0xff)); s.push_back(static_cast<char>((v >> 8) & 0xff)); }
void     put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }

// 16x16 32-bit BGRA bottom-up BMP from a decoded image, nearest neighbour when the image is smaller.
std::string bmp_from_gimage(const gimage& img)
{
    if(img.w <= 0 || img.h <= 0 || img.px.empty()) return "";
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
            int    sx = x * img.w / ICON, sy = y * img.h / ICON;
            size_t o  = static_cast<size_t>(sy) * img.w + sx;
            bool   on = img.opaque_at(o);
            out.push_back(static_cast<char>(img.px[3 * o + 2]));
            out.push_back(static_cast<char>(img.px[3 * o + 1]));
            out.push_back(static_cast<char>(img.px[3 * o]));
            out.push_back(static_cast<char>(on ? 255 : 0));
        }
    return out;
}

// Decode any supported image file to at most ICON x ICON.
bool decode_small(const std::string& file, gimage& img)
{
    int w = 0, h = 0;
    int kind = sniff_size(file, w, h);
    if(!kind || w <= 0 || h <= 0) return false;
    return decode_image(kind, file, img, std::min(w, ICON), std::min(h, ICON));
}

// ICO: a directory of images, each a PNG or a BMP without the file header and with its height doubled
// for the AND mask. The entry nearest to ICON pixels wins.
bool decode_ico(const std::string& file, gimage& img)
{
    auto   u = reinterpret_cast<const unsigned char*>(file.data());
    size_t n = file.size();
    if(n < 6 || le16(u) != 0 || le16(u + 2) != 1) return false;
    int    count = static_cast<int>(le16(u + 4));
    int    best = -1, best_score = 1 << 30;
    for(int i = 0; i < count && 6 + 16 * (i + 1) <= static_cast<int>(n); i++)
    {
        const unsigned char* e  = u + 6 + 16 * i;
        int                  w  = e[0] ? e[0] : 256;
        uint32_t             sz = le32(e + 8), off = le32(e + 12);
        if(off + sz > n || sz < 16) continue;
        int score = w >= ICON ? (w - ICON) : (ICON - w) * 4 + 1000; // at least ICON wide, else the largest below
        if(score < best_score)
        {
            best_score = score;
            best       = i;
        }
    }
    if(best < 0) return false;
    const unsigned char* e   = u + 6 + 16 * best;
    uint32_t             sz  = le32(e + 8), off = le32(e + 12);
    std::string          sub = file.substr(off, sz);
    if(sub.size() > 8 && sub[0] == static_cast<char>(0x89) && sub[1] == 'P') return decode_small(sub, img);
    // BMP entry: BITMAPINFOHEADER follows, height counts the XOR and the AND mask
    auto     b = reinterpret_cast<const unsigned char*>(sub.data());
    if(sub.size() < 40 || le32(b) != 40) return false;
    int      bpp     = static_cast<int>(le16(b + 14));
    uint32_t clr     = le32(b + 32);
    uint32_t palette = bpp <= 8 ? (clr ? clr : (1u << bpp)) * 4 : 0;
    std::string bmp  = "BM";
    put32(bmp, 14 + static_cast<uint32_t>(sub.size()));
    put32(bmp, 0);
    put32(bmp, 14 + 40 + palette);
    bmp += sub;
    // halve the height field, which is at byte 8 of the info header (offset 14 in the file)
    int32_t hh = static_cast<int32_t>(le32(b + 8)) / 2;
    bmp[14 + 8]  = static_cast<char>(hh & 0xff);
    bmp[14 + 9]  = static_cast<char>((hh >> 8) & 0xff);
    bmp[14 + 10] = static_cast<char>((hh >> 16) & 0xff);
    bmp[14 + 11] = static_cast<char>((hh >> 24) & 0xff);
    int w = 0, h = 0;
    if(!sniff_bmp(bmp, w, h) || w <= 0 || h <= 0) return false;
    return decode_bmp(bmp, img, std::min(w, ICON), std::min(h, ICON));
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
    uint32_t tl = 0, bl = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, MAGIC, 4) == 0 && fread(&tl, 4, 1, f) == 1 &&
              fread(&bl, 4, 1, f) == 1 && tl < 512 && bl < 8192;
    if(ok)
    {
        out.title.resize(tl);
        out.icon_bmp.resize(bl);
        ok = (tl == 0 || fread(&out.title[0], 1, tl, f) == tl) && (bl == 0 || fread(&out.icon_bmp[0], 1, bl, f) == bl);
    }
    fclose(f);
    if(!ok) out = tab_info();
    return ok;
}

bool tabs_save(const std::string& url, const tab_info& t)
{
    mkdir(TAB_DIR, 0777);
    FILE* f = fopen(path_for(url).c_str(), "wb");
    if(!f)
    {
        ESP_LOGW(TAG, "cannot write %s", path_for(url).c_str());
        return false;
    }
    uint32_t tl = t.title.size(), bl = t.icon_bmp.size();
    bool     ok = fwrite(MAGIC, 1, 4, f) == 4 && fwrite(&tl, 4, 1, f) == 1 && fwrite(&bl, 4, 1, f) == 1 &&
              fwrite(t.title.data(), 1, tl, f) == tl && fwrite(t.icon_bmp.data(), 1, bl, f) == bl;
    if(fclose(f) != 0) ok = false;
    return ok;
}

std::string tabs_icon_bmp(const std::string& file)
{
    gimage img;
    bool   ok = false;
    auto   u  = reinterpret_cast<const unsigned char*>(file.data());
    if(file.size() >= 6 && le16(u) == 0 && le16(u + 2) == 1) ok = decode_ico(file, img);
    else ok = decode_small(file, img);
    return ok ? bmp_from_gimage(img) : "";
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
    // 4 px above the tabs, a 3 px rule under them, the tab fills the rest
    int inner = tab_h - 4 - 3;
    char css[700];
    snprintf(css, sizeof(css),
             "body{margin:0;padding:0;background:#fff}"
             ".bar{display:flex;height:%dpx;padding:4px 6px 0 6px;border-bottom:3px solid #000;box-sizing:border-box}"
             ".tab{flex:1 1 0;min-width:0;height:%dpx;margin-right:5px;border:3px solid #000;border-bottom:none;"
             "border-radius:10px 10px 0 0;padding:0 7px;box-sizing:border-box;font-family:Arial;font-weight:bold;"
             "font-size:17px;line-height:%dpx;white-space:nowrap;overflow:hidden;background:#fff;color:#000}"
             ".tab.cur{background:#000;color:#fff}"
             ".tab img{width:16px;height:16px;vertical-align:middle;margin-right:5px}",
             tab_h, inner, inner - 6);
    std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><style>";
    html += css;
    html += "</style></head><body><div class=\"bar\">";
    for(size_t i = 0; i < pages.size(); i++)
    {
        const tab_info* t = i < infos.size() ? &infos[i] : nullptr;
        html += i == current ? "<div class=\"tab cur\">" : "<div class=\"tab\">";
        if(t && !t->icon_bmp.empty()) html += "<img src=\"data:image/bmp;base64," + b64_encode(t->icon_bmp) + "\">";
        html += html_escape(t && !t->title.empty() ? t->title : tabs_host(pages[i].url));
        html += "</div>";
    }
    html += "</div></body></html>";
    return html;
}
