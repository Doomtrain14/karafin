#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace karafin::jellyfin {

// Settings keys (owned by this module):
//   jellyfin.max_streaming_bitrate  int, bits/s (default 120'000'000). Above this the server transcodes.
//   jellyfin.max_audio_channels     int (default 8; mpv downmixes for the console speakers)
//   jellyfin.enable_av1             bool (default true; software dav1d, capped at 1080p)
// When the server transcodes, its target bitrate is min(source, maxStreamingBitrate).
// The server's DTOs hold bitrates as int32.
inline constexpr int64_t kMaxProfileBitrate = 2'000'000'000;

struct DeviceProfileOptions {
    int64_t maxStreamingBitrate = 120'000'000;
    int64_t maxStaticBitrate = 200'000'000;
    int maxAudioChannels = 8;
    bool enableAv1 = true;
    int av1MaxWidth = 1920;
    int av1MaxHeight = 1080;

    static DeviceProfileOptions fromSettings();
};

// Jellyfin DeviceProfile describing what mpv + nvtegra on the Switch can direct play.
nlohmann::json buildDeviceProfile(const DeviceProfileOptions& options = DeviceProfileOptions::fromSettings());

}  // namespace karafin::jellyfin
