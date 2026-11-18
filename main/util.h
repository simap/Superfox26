// Header-only helpers shared across the program: the fnv1a64 string hash, little-endian reads of
// 16-bit and 32-bit integers, Base64 encode and decode, and extraction of the bytes from a base64
// data URL.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

inline uint64_t fnv1a64(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for(unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

inline uint32_t le16(const uint8_t* p)
{
    return p[0] | (p[1] << 8);
}
inline uint32_t le32(const uint8_t* p)
{
    return le16(p) | (le16(p + 2) << 16);
}

inline const char* base64_alphabet()
{
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

inline std::string base64_encode(const std::string& in)
{
    const char* b64 = base64_alphabet();
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

inline std::string base64_decode(const std::string& in)
{
    const char* b64 = base64_alphabet();
    std::string out;
    int         val = 0, bits = -8;
    for(unsigned char c : in)
    {
        const char* p = c ? strchr(b64, c) : nullptr;
        if(!p) continue;
        val = (val << 6) + static_cast<int>(p - b64);
        bits += 6;
        if(bits >= 0)
        {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

inline bool data_url_bytes(const std::string& url, std::string& out)
{
    const auto comma = url.find(',');
    if(comma == std::string::npos || url.rfind(";base64", comma) == std::string::npos) return false;
    out = base64_decode(url.substr(comma + 1));
    return true;
}
