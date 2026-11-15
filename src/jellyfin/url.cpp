#include "jellyfin/url.hpp"

#include <algorithm>
#include <cctype>

namespace karafin::jellyfin {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

struct Parts {
    std::string scheme;  // "" when absent
    std::string host;    // may include [ipv6]
    std::string port;    // "" when absent
    std::string path;    // "" or "/sub/path" without trailing slash
};

bool splitUrl(std::string_view in, Parts& out) {
    in = trim(in);
    if (in.empty()) return false;
    const auto schemeEnd = in.find("://");
    if (schemeEnd != std::string_view::npos) {
        out.scheme = lower(in.substr(0, schemeEnd));
        in.remove_prefix(schemeEnd + 3);
        if (out.scheme != "http" && out.scheme != "https") return false;
    }
    // Drop query / fragment.
    const auto cut = in.find_first_of("?#");
    if (cut != std::string_view::npos) in = in.substr(0, cut);

    const auto slash = in.find('/');
    std::string_view authority = in.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view() : in.substr(slash);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (authority.empty()) return false;

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        out.host = lower(authority.substr(0, close + 1));
        std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            out.port = std::string(rest.substr(1));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            out.host = lower(authority.substr(0, colon));
            out.port = std::string(authority.substr(colon + 1));
        } else {
            out.host = lower(authority);
        }
    }
    if (out.host.empty()) return false;
    for (char c : out.host)
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    for (char c : out.port)
        if (c < '0' || c > '9') return false;
    if (!out.port.empty()) {
        if (out.port.size() > 5) return false;
        const long p = std::stol(out.port);
        if (p <= 0 || p > 65535) return false;
    }

    std::string p(path);
    // Pasted web-client links: everything from a "/web" segment on is UI, not API.
    const std::string lp = lower(p);
    for (size_t pos = lp.find("/web"); pos != std::string::npos; pos = lp.find("/web", pos + 1)) {
        if (lp.size() == pos + 4 || lp[pos + 4] == '/') {
            p.erase(pos);
            break;
        }
    }
    while (!p.empty() && p.back() == '/') p.pop_back();
    out.path = p;
    return true;
}

std::string build(const std::string& scheme, const Parts& parts, const std::string& port) {
    std::string out = scheme + "://" + parts.host;
    const bool defaultPort = (scheme == "http" && port == "80") || (scheme == "https" && port == "443");
    if (!port.empty() && !defaultPort) out += ":" + port;
    out += parts.path;
    return out;
}

}  // namespace

std::string normalizeBaseUrl(std::string_view url) {
    Parts parts;
    if (!splitUrl(url, parts) || parts.scheme.empty()) return {};
    return build(parts.scheme, parts, parts.port);
}

std::vector<std::string> serverUrlCandidates(std::string_view input) {
    std::vector<std::string> out;
    Parts parts;
    if (!splitUrl(input, parts)) return out;
    auto push = [&out](std::string u) {
        if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(std::move(u));
    };
    if (!parts.scheme.empty()) {
        push(build(parts.scheme, parts, parts.port));
        return out;
    }
    if (!parts.port.empty()) {
        if (parts.port == "443" || parts.port == "8920") {
            push(build("https", parts, parts.port));
            push(build("http", parts, parts.port));
        } else {
            push(build("http", parts, parts.port));
            push(build("https", parts, parts.port));
        }
        return out;
    }
    push(build("https", parts, ""));
    push(build("http", parts, "8096"));
    push(build("http", parts, ""));
    return out;
}

std::string stripApiPath(std::string_view url, std::string_view apiPath) {
    std::string u(url);
    if (const auto q = u.find_first_of("?#"); q != std::string::npos) u.erase(q);
    const std::string lu = lower(u);
    const std::string la = lower(apiPath);
    if (lu.size() >= la.size() && lu.compare(lu.size() - la.size(), la.size(), la) == 0) u.erase(u.size() - la.size());
    return normalizeBaseUrl(u);
}

}  // namespace karafin::jellyfin
