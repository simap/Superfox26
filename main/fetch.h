// HTTP GET into memory over esp_http_client. One client handle per scheme is kept, so
// requests to the same host reuse the connection.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct fetch_result
{
    int         status    = 0;     // HTTP status of the final response, 0 on transport failure
    bool        truncated = false; // reading stopped at max_bytes before the body ended
    bool        partial   = false; // out holds a prefix of the resource: a 206 answer or a truncated read
    int64_t     total     = -1;    // size of the whole resource from Content-Length or Content-Range, -1 unknown
    std::string final_url;         // URL after redirects
};

// Initialise the client. Call once before the first fetch.
void fetch_init();

// GET url into out, following up to 8 redirects, across schemes too. Reading stops at max_bytes with
// truncated set and the first max_bytes kept; the connection is then dropped, so a truncated fetch
// costs the next request to that host a reconnect. Returns true for a 2xx response with the complete
// body. Callable from any task; fetches run one at a time.
bool fetch_url(const std::string& url, std::string& out, size_t max_bytes, fetch_result* res = nullptr);

// GET the first n bytes of url with a Range request. A server that ignores Range answers 200 and the
// read stops at n. Returns true when out holds the first min(n, total) bytes of the resource; res->total
// is set when the server said how large it is.
bool fetch_prefix(const std::string& url, std::string& out, size_t n, fetch_result* res = nullptr);
