#pragma once

// Short labels for the stream mpv is playing, shown as tags in the player's top bar
// ("Direct Play", "1080p", "HEVC 10-bit", "EAC3 5.1").

#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace karafin::player {

inline std::string upperCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Bit depth from an FFmpeg pixel format name; 8 when it can't be told.
inline int pixelFormatBitDepth(std::string_view fmt) {
    if (fmt.starts_with("p010")) return 10;
    if (fmt.starts_with("p012")) return 12;
    if (fmt.find("p10") != std::string_view::npos) return 10;  // yuv420p10le, ...
    if (fmt.find("p12") != std::string_view::npos) return 12;
    return 8;
}

inline std::string videoCodecLabel(std::string_view codec, std::string_view pixelFormat) {
    if (codec.empty()) return {};
    std::string name;
    if (codec == "h264") name = "H.264";
    else if (codec == "hevc" || codec == "h265") name = "HEVC";
    else if (codec == "mpeg2video") name = "MPEG-2";
    else if (codec == "mpeg4") name = "MPEG-4";
    else if (codec == "vc1") name = "VC-1";
    else name = upperCopy(codec);
    const int depth = pixelFormatBitDepth(pixelFormat);
    if (depth > 8) name += std::format(" {}-bit", depth);
    return name;
}

// Classified by either dimension so letterboxed (e.g. 3840x1600) video keeps its class.
inline std::string resolutionLabel(int64_t width, int64_t height) {
    if (width <= 0 || height <= 0) return {};
    if (width >= 3200 || height >= 2000) return "4K";
    if (width >= 2200 || height >= 1300) return "1440p";
    if (width >= 1700 || height >= 1000) return "1080p";
    if (width >= 1200 || height >= 700) return "720p";
    return std::format("{}p", height);
}

inline std::string audioCodecLabel(std::string_view codec, int64_t channels) {
    if (codec.empty()) return {};
    std::string name;
    if (codec == "truehd") name = "TrueHD";
    else if (codec == "opus") name = "Opus";
    else if (codec == "vorbis") name = "Vorbis";
    else if (codec.starts_with("pcm_")) name = "PCM";
    else if (codec.starts_with("dts")) name = "DTS";
    else name = upperCopy(codec);
    if (channels == 1) name += " Mono";
    else if (channels == 2) name += " Stereo";
    else if (channels == 6) name += " 5.1";
    else if (channels == 8) name += " 7.1";
    else if (channels > 0) name += std::format(" {} ch", channels);
    return name;
}

}  // namespace karafin::player
