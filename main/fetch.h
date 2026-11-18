// HTTP client that fetches the body of an http:// or https:// URL into a string, following
// redirects up to a fixed limit.
// fetch_url returns true only when the whole body arrived within max_bytes without truncation;
// fetch_prefix asks for just the first n bytes with a Range request.
// Call fetch_init once before any fetch.
// Fetches are serialized, so one runs at a time and any task may call them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct fetch_result
{
    int         status    = 0;
    bool        truncated = false;
    bool        partial   = false;
    int64_t     total     = -1;
    std::string final_url;
};

bool fetch_init();

bool fetch_url(const std::string& url, std::string& out, size_t max_bytes, fetch_result* res = nullptr);

bool fetch_prefix(const std::string& url, std::string& out, size_t n, fetch_result* res = nullptr);
