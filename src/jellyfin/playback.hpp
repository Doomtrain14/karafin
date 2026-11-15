#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "jellyfin/models.hpp"

namespace karafin::jellyfin {

enum class PlayMethod { DirectPlay, DirectStream, Transcode };
std::string_view playMethodName(PlayMethod m);           // "DirectPlay", "DirectStream", "Transcode"
PlayMethod parsePlayMethod(std::string_view s);

// Input for Client::playbackPlan (POST /Items/{id}/PlaybackInfo).
struct PlaybackRequest {
    std::string itemId;
    std::string mediaSourceId;            // "" -> server default (first source)
    int64_t startTimeTicks = 0;
    std::optional<int> audioStreamIndex;
    std::optional<int> subtitleStreamIndex;  // -1 = subtitles off
    std::optional<int64_t> maxStreamingBitrate;  // default: DeviceProfileOptions::fromSettings()
    bool enableDirectPlay = true;
    bool enableDirectStream = true;
    bool enableTranscoding = true;
    bool autoOpenLiveStream = true;
};

// An external subtitle mpv can load with `sub-add <url>`.
struct ExternalSubtitle {
    int index = -1;            // Jellyfin MediaStream index (use for reporting SubtitleStreamIndex)
    std::string url;           // absolute, includes ApiKey
    std::string format;        // "srt", "ass", "vtt", ...
    std::string language;
    std::string title;         // DisplayTitle
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
};

// Resolved playback decision: what mpv should open and how to report it.
struct PlaybackPlan {
    std::string itemId;
    std::string mediaSourceId;
    std::string playSessionId;
    std::string liveStreamId;
    std::string url;           // absolute URL for mpv (static stream or HLS master playlist)
    PlayMethod method = PlayMethod::DirectPlay;
    MediaSource source;        // the chosen MediaSource as returned by PlaybackInfo
    std::vector<ExternalSubtitle> externalSubtitles;
    int audioStreamIndex = -1;     // effective selection (request or source default)
    int subtitleStreamIndex = -1;  // -1 = off
    int64_t startTimeTicks = 0;

    bool isTranscoding() const { return method == PlayMethod::Transcode; }
    // mpv track id ("aid"/"sid"/"vid", 1-based) of an embedded stream, counting only non-external
    // streams of that type in container order. -1 if the stream is external/unknown or the plan
    // is a transcode (HLS output carries a single audio track and burned-in/external subtitles).
    int mpvTrackId(int jellyfinStreamIndex) const;
    const ExternalSubtitle* externalSubtitle(int jellyfinStreamIndex) const;
};

// Body for /Sessions/Playing, /Sessions/Playing/Progress and /Sessions/Playing/Stopped.
struct PlaybackReport {
    std::string itemId;
    std::string mediaSourceId;
    std::string playSessionId;
    std::string liveStreamId;
    int64_t positionTicks = 0;
    bool isPaused = false;
    bool isMuted = false;
    int audioStreamIndex = -1;
    int subtitleStreamIndex = -1;
    PlayMethod playMethod = PlayMethod::DirectPlay;
    int volumeLevel = 100;
    double playbackRate = 1.0;
    std::string eventName;     // optional: "timeupdate", "pause", "unpause", "audiotrackchange", ...
    bool failed = false;       // Stopped only: playback ended because of an error

    static PlaybackReport fromPlan(const PlaybackPlan& plan, int64_t positionTicks = 0);
    nlohmann::json toJson() const;
    static PlaybackReport fromJson(const nlohmann::json& j);
};

}  // namespace karafin::jellyfin
