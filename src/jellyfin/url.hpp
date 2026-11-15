#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace karafin::jellyfin {

// Candidate base URLs for what the user typed, most likely first. Handles missing scheme
// (https first unless a non-TLS port is given, then http with :8096), trailing slashes, pasted
// web-UI links ("/web/index.html#!/home"), and keeps reverse-proxy sub-paths ("/jellyfin").
// Returns an empty list when the input cannot be a URL.
std::vector<std::string> serverUrlCandidates(std::string_view input);

// Canonical form of an absolute base URL: lower-case scheme/host, no trailing slash, no query,
// fragment or "/web" suffix. Returns "" for unusable input.
std::string normalizeBaseUrl(std::string_view url);

// Strips a known API path suffix from an effective (post-redirect) URL:
// "https://h/jf/System/Info/Public" -> "https://h/jf".
std::string stripApiPath(std::string_view url, std::string_view apiPath);

}  // namespace karafin::jellyfin
