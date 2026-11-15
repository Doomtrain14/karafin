#include "downloads/detail/util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
#include <format>
#include <vector>

#include "core/paths.hpp"

namespace karafin::downloads::detail {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string extensionOf(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) return {};
    std::string ext = lower(name.substr(dot + 1));
    if (ext.size() > 5) return {};
    for (char c : ext)
        if (!std::isalnum(static_cast<unsigned char>(c))) return {};
    return ext;
}

bool knownVideoExtension(std::string_view ext) {
    static constexpr std::string_view kKnown[] = {"mkv", "mp4", "m4v", "mov", "webm", "avi", "ts", "m2ts",
                                                  "mts", "mpg", "mpeg", "wmv", "flv", "ogv", "3gp", "vob"};
    return std::find(std::begin(kKnown), std::end(kKnown), ext) != std::end(kKnown);
}

const Json* field(const Json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    return &*it;
}

}  // namespace

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

std::string jstr(const Json& j, const char* key, std::string fallback) {
    const Json* v = field(j, key);
    return v && v->is_string() ? v->get<std::string>() : fallback;
}

int64_t jint(const Json& j, const char* key, int64_t fallback) {
    const Json* v = field(j, key);
    if (!v) return fallback;
    if (v->is_number_integer()) return v->get<int64_t>();
    if (v->is_number()) return static_cast<int64_t>(v->get<double>());
    return fallback;
}

uint64_t juint(const Json& j, const char* key, uint64_t fallback) {
    const Json* v = field(j, key);
    if (!v) return fallback;
    if (v->is_number_unsigned()) return v->get<uint64_t>();
    if (v->is_number_integer()) {
        const int64_t i = v->get<int64_t>();
        return i < 0 ? fallback : static_cast<uint64_t>(i);
    }
    if (v->is_number()) {
        const double d = v->get<double>();
        return d < 0 ? fallback : static_cast<uint64_t>(d);
    }
    return fallback;
}

double jnum(const Json& j, const char* key, double fallback) {
    const Json* v = field(j, key);
    return v && v->is_number() ? v->get<double>() : fallback;
}

bool jbool(const Json& j, const char* key, bool fallback) {
    const Json* v = field(j, key);
    return v && v->is_boolean() ? v->get<bool>() : fallback;
}

std::string mediaExtension(std::string_view container, std::string_view dispositionName, std::string_view serverPath) {
    if (std::string ext = extensionOf(dispositionName); !ext.empty()) return ext;
    if (std::string ext = extensionOf(serverPath); knownVideoExtension(ext)) return ext;

    std::vector<std::string> parts;
    std::string c = lower(container);
    size_t start = 0;
    while (start <= c.size()) {
        const size_t comma = c.find(',', start);
        std::string part = c.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char ch) { return std::isspace(ch); }),
                   part.end());
        if (!part.empty()) parts.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    // ffprobe reports mp4-family files as "mov,mp4,m4a,3gp,3g2,mj2".
    if (std::find(parts.begin(), parts.end(), "mp4") != parts.end()) return "mp4";
    if (std::find(parts.begin(), parts.end(), "matroska") != parts.end()) return "mkv";
    for (const auto& p : parts)
        if (knownVideoExtension(p)) return p;
    if (!parts.empty() && parts.front().size() <= 5) return parts.front();
    return "mkv";
}

std::optional<ContentRange> parseContentRange(std::string_view h) {
    while (!h.empty() && std::isspace(static_cast<unsigned char>(h.front()))) h.remove_prefix(1);
    if (!h.starts_with("bytes")) return std::nullopt;
    h.remove_prefix(5);
    while (!h.empty() && (h.front() == ' ' || h.front() == '=')) h.remove_prefix(1);
    const auto dash = h.find('-');
    const auto slash = h.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || slash < dash) return std::nullopt;
    ContentRange r;
    auto num = [](std::string_view s, uint64_t& out) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        if (s.empty()) return false;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc{} && p == s.data() + s.size();
    };
    if (!num(h.substr(0, dash), r.start) || !num(h.substr(dash + 1, slash - dash - 1), r.end)) return std::nullopt;
    std::string_view total = h.substr(slash + 1);
    if (total != "*" && !num(total, r.total)) return std::nullopt;
    if (r.end < r.start) return std::nullopt;
    return r;
}

double backoffSeconds(int attempt) {
    if (attempt < 1) attempt = 1;
    double s = 2.0;
    for (int i = 1; i < attempt && s < 120.0; ++i) s *= 2.0;
    return std::min(s, 120.0);
}

std::string formatBytes(uint64_t bytes) {
    constexpr double kKiB = 1024.0;
    const double b = static_cast<double>(bytes);
    if (b >= kKiB * kKiB * kKiB) return std::format("{:.1f} GB", b / (kKiB * kKiB * kKiB));
    if (b >= kKiB * kKiB) return std::format("{:.0f} MB", b / (kKiB * kKiB));
    if (b >= kKiB) return std::format("{:.0f} KB", b / kKiB);
    return std::format("{} B", bytes);
}

bool countsAsPlayed(int64_t positionTicks, int64_t runTimeTicks) {
    return runTimeTicks > 0 && positionTicks >= runTimeTicks * 9 / 10;
}

void patchUserData(Json& metadata, int64_t positionTicks, bool played) {
    if (!metadata.is_object()) return;
    Json& item = metadata["item"];
    if (!item.is_object()) item = Json::object();
    Json& ud = item["UserData"];
    if (!ud.is_object()) ud = Json::object();
    ud["PlaybackPositionTicks"] = played ? 0 : positionTicks;
    ud["Played"] = played;
    const int64_t runtime = jint(item, "RunTimeTicks", 0);
    if (played) {
        ud["PlayedPercentage"] = 100.0;
    } else if (runtime > 0 && positionTicks > 0) {
        ud["PlayedPercentage"] = 100.0 * static_cast<double>(positionTicks) / static_cast<double>(runtime);
    } else {
        ud.erase("PlayedPercentage");
    }
}

std::string serverDir(const std::string& serverId) { return paths::downloadsDir() + "/" + serverId; }

std::string itemDir(const std::string& serverId, const std::string& itemId) {
    return serverDir(serverId) + "/" + itemId;
}

std::string queueFile() { return paths::downloadsDir() + "/queue.json"; }

}  // namespace karafin::downloads::detail
