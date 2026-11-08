#include "url.h"

#include <cctype>
#include <vector>

namespace
{
bool has_scheme(const std::string& s)
{
    size_t i = 0;
    if(i >= s.size() || !isalpha(static_cast<unsigned char>(s[0]))) return false;
    while(i < s.size() && (isalnum(static_cast<unsigned char>(s[i])) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    return i < s.size() && s[i] == ':';
}

// scheme://host part of an absolute URL, without the path
std::string origin_of(const std::string& url)
{
    auto p = url.find("://");
    if(p == std::string::npos) return url;
    auto q = url.find('/', p + 3);
    return q == std::string::npos ? url : url.substr(0, q);
}

// Removes "." and ".." segments from an absolute URL path.
std::string normalize_path(const std::string& path)
{
    std::vector<std::string> seg;
    size_t                   i = 0;
    while(i <= path.size())
    {
        size_t      j = path.find('/', i);
        std::string s = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if(s == "..")
        {
            if(!seg.empty()) seg.pop_back();
        } else if(s != "." && !(s.empty() && i != 0 && j != std::string::npos))
            seg.push_back(s);
        if(j == std::string::npos) break;
        i = j + 1;
    }
    std::string out;
    for(size_t k = 0; k < seg.size(); k++)
    {
        if(k) out += '/';
        out += seg[k];
    }
    if(path.size() && path.back() == '/' && (out.empty() || out.back() != '/')) out += '/';
    return out.empty() || out[0] != '/' ? "/" + out : out;
}
} // namespace

std::string resolve_url(const std::string& base, const std::string& rel0)
{
    std::string rel = rel0;
    while(!rel.empty() && isspace(static_cast<unsigned char>(rel.back()))) rel.pop_back();
    size_t ws = 0;
    while(ws < rel.size() && isspace(static_cast<unsigned char>(rel[ws]))) ws++;
    rel = rel.substr(ws);
    if(rel.empty()) return "";
    if(rel.compare(0, 5, "data:") == 0) return rel;
    std::string out;
    if(has_scheme(rel)) out = rel;
    else if(rel.compare(0, 2, "//") == 0)
    {
        auto p = base.find("://");
        out    = (p == std::string::npos ? std::string("https") : base.substr(0, p)) + ":" + rel;
    } else if(rel[0] == '/') out = origin_of(base) + rel;
    else
    {
        // relative to the directory of the base path, query and fragment of the base dropped
        std::string b  = base;
        auto        qf = b.find_first_of("?#");
        if(qf != std::string::npos) b = b.substr(0, qf);
        auto        o     = origin_of(b);
        std::string path  = b.size() > o.size() ? b.substr(o.size()) : "/";
        auto        slash = path.find_last_of('/');
        path              = slash == std::string::npos ? "/" : path.substr(0, slash + 1);
        auto        rq    = rel.find_first_of("?#");
        std::string rpath = rq == std::string::npos ? rel : rel.substr(0, rq);
        std::string rtail = rq == std::string::npos ? "" : rel.substr(rq);
        out               = o + normalize_path(path + rpath) + rtail;
    }
    auto h = out.find('#');
    if(h != std::string::npos) out = out.substr(0, h);
    if(out.compare(0, 7, "http://") != 0 && out.compare(0, 8, "https://") != 0) return "";
    return out;
}
