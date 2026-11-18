#pragma once

#include <string>

std::string resolve_url(const std::string& base, const std::string& rel);

std::string url_origin(const std::string& url);
