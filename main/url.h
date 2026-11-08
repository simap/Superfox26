// URL resolution, RFC 3986 reference resolution for the http and https cases the browser meets.
#pragma once

#include <string>

// Absolute URL for rel against base. Fragments are dropped, queries kept. Leading and trailing
// whitespace in rel is ignored. Returns "" for schemes other than http, https and data (data URLs are
// returned as they are).
std::string resolve_url(const std::string& base, const std::string& rel);
