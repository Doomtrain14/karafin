#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "jellyfin/models.hpp"

namespace karafin::jellyfin {

enum class ImageType { Primary, Backdrop, Thumb, Logo, Banner, Art, Disc };
std::string_view imageTypeName(ImageType t);

struct ImageOptions {
    int maxWidth = 0;      // 0 = not sent
    int maxHeight = 0;
    int quality = 90;      // JPEG quality
    int index = 0;         // backdrop index
    bool fallback = true;  // use parent / series images when the item has none of this type
};

// Result of Client::probeDownload: what /Items/{id}/Download will deliver.
struct DownloadProbe {
    std::string url;               // includes ApiKey
    uint64_t contentLength = 0;    // 0 if the server did not say
    std::string eTag;              // compare on resume; changed -> restart from 0
    std::string lastModified;
    std::string contentType;
    std::string fileName;          // from Content-Disposition, may be empty
    bool acceptRanges = false;     // server honours "Range: bytes=N-"
};

// Raw JSON for the offline store plus the parsed item. series/season are null for movies.
struct OfflineMetadata {
    nlohmann::json item;
    nlohmann::json series;
    nlohmann::json season;
    BaseItem parsed;
};

// One external subtitle file to bundle with a download.
struct SubtitleDownload {
    int index = -1;
    std::string url;               // absolute, includes ApiKey
    std::string format;            // file extension: "srt", "ass", "vtt", ...
    std::string language;
    std::string title;
    std::string fileName;          // suggested: "<index>.<lang>[.forced].<format>"
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
};

// Result of Client::downloadRemoteSubtitleAndRefresh.
struct SubtitleAddResult {
    MediaSource source;                    // refreshed source with the new stream
    std::optional<MediaStream> newStream;  // the subtitle that appeared (nullopt if not yet indexed)
    std::string url;                       // sub-add URL for newStream ("" when nullopt)
};

}  // namespace karafin::jellyfin
