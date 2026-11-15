#pragma once

// Bridge to the downloads module (offline files, local progress, streaming throttle). Kept
// behind this small interface so the player builds even when the downloads module is absent.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "jellyfin/models.hpp"

namespace karafin::player::offline {

struct LocalSubtitle {
    int index = -1;  // Jellyfin MediaStream index
    std::string path;
    std::string language;
    std::string title;
    std::string format;
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
};

struct Media {
    std::string key;        // downloads key ("<serverId>:<itemId>")
    std::string mediaPath;  // local file for mpv
    jellyfin::BaseItem item;
    int64_t positionTicks = 0;
    std::vector<LocalSubtitle> subtitles;
};

bool available();
// Completed download of `itemId` (any server).
std::optional<Media> find(const std::string& itemId);
// Next downloaded episode of the same series.
std::optional<Media> next(const std::string& key);
// Local resume point; `queueReport` also queues it for the server (flushed when reachable).
void saveProgress(const std::string& key, int64_t positionTicks, bool finished, bool queueReport);
// Download bandwidth cap while a streamed (non-local) video plays.
void setStreamingThrottle(bool throttled);

}  // namespace karafin::player::offline
