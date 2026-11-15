#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Internal helpers of the downloads module (pure functions; covered by downloads::selfTest).
namespace karafin::downloads::detail {

using Json = nlohmann::json;

inline constexpr uint64_t kFat32MaxFileSize = 0xFFFFFFFFull;          // 4 GiB - 1
inline constexpr uint64_t kSpaceMargin = 256ull * 1024 * 1024;         // keep free on the SD card
inline constexpr int kMaxAttempts = 8;                                 // consecutive failures -> Failed
inline constexpr uint64_t kProgressResetBytes = 1024ull * 1024;        // an attempt this productive resets the count

int64_t unixNow();

// dump() that never throws (invalid UTF-8 from file names / server strings becomes U+FFFD).
inline std::string dumpJson(const Json& j, int indent = -1) {
    return j.dump(indent, ' ', false, Json::error_handler_t::replace);
}

// Lenient JSON getters (missing / null / wrong type -> fallback).
std::string jstr(const Json& j, const char* key, std::string fallback = {});
int64_t jint(const Json& j, const char* key, int64_t fallback = 0);
uint64_t juint(const Json& j, const char* key, uint64_t fallback = 0);
double jnum(const Json& j, const char* key, double fallback = 0);
bool jbool(const Json& j, const char* key, bool fallback = false);

// File extension (without dot, lower case) for the media file: the Content-Disposition file
// name wins, then the server path, then the container list ("mov,mp4,m4a,..." -> "mp4").
// Falls back to "mkv".
std::string mediaExtension(std::string_view container, std::string_view dispositionName = {},
                           std::string_view serverPath = {});

// "bytes 100-199/1000" -> {100, 199, 1000}; total 0 when "*".
struct ContentRange {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t total = 0;
};
std::optional<ContentRange> parseContentRange(std::string_view header);

// Delay before retry `attempt` (1-based): 2, 4, 8, ... capped at 120 s.
double backoffSeconds(int attempt);

// "1.4 GB", "830 MB", "12 KB".
std::string formatBytes(uint64_t bytes);

// Weak ETags (W/"...") cannot be used with If-Range.
inline bool isStrongETag(std::string_view etag) { return !etag.empty() && !etag.starts_with("W/"); }

// Writes local progress into a stored metadata document ({"item": {...}, ...}): UserData
// PlaybackPositionTicks / Played / PlayedPercentage.
void patchUserData(Json& metadata, int64_t positionTicks, bool played);

// True when playback at `positionTicks` of `runTimeTicks` counts as watched (>= 90%).
bool countsAsPlayed(int64_t positionTicks, int64_t runTimeTicks);

// Folder layout (sdmc:/switch/karafin/downloads/<serverId>/<itemId>/...).
std::string serverDir(const std::string& serverId);
std::string itemDir(const std::string& serverId, const std::string& itemId);
std::string queueFile();
inline constexpr const char* kMetadataFile = "metadata.json";
inline constexpr const char* kManifestFile = "download.json";
inline constexpr const char* kPosterFile = "poster.jpg";
inline constexpr const char* kBackdropFile = "backdrop.jpg";
inline constexpr const char* kSeriesPosterFile = "series_poster.jpg";
inline constexpr const char* kSubtitleDir = "subtitles";
inline constexpr const char* kPartSuffix = ".part";

}  // namespace karafin::downloads::detail
