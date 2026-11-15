#include "screens/item_meta.hpp"

#include <format>

#include "screens/common.hpp"

namespace karafin::screens {

namespace jf = karafin::jellyfin;

namespace {

bool contains(std::string_view hay, std::string_view needle) {
    return lowerAscii(hay).find(lowerAscii(needle)) != std::string::npos;
}

std::string channelsText(int channels) {
    switch (channels) {
        case 0: return {};
        case 1: return "Mono";
        case 2: return "Stereo";
        case 6: return "5.1";
        case 7: return "6.1";
        case 8: return "7.1";
        default: return std::format("{}ch", channels);
    }
}

std::string audioCodecName(const jf::MediaStream& s) {
    const std::string c = lowerAscii(s.codec);
    if (contains(s.profile, "atmos") || contains(s.displayTitle, "atmos") || contains(s.title, "atmos")) return "Atmos";
    if (c == "truehd") return "TrueHD";
    if (c == "eac3") return "DD+";
    if (c == "ac3") return "Dolby Digital";
    if (c == "dts") return contains(s.profile, "ma") ? "DTS-HD MA" : "DTS";
    if (c == "aac") return "AAC";
    if (c == "flac") return "FLAC";
    if (c == "opus") return "Opus";
    if (c == "mp3") return "MP3";
    if (c == "vorbis") return "Vorbis";
    if (c.rfind("pcm", 0) == 0) return "PCM";
    return upperAscii(c);
}

}  // namespace

std::vector<Chip> itemChips(const jf::BaseItem& item) {
    std::vector<Chip> out;
    if (auto y = yearText(item); !y.empty()) out.push_back({y, ChipStyle::Text});
    if (!item.officialRating.empty()) out.push_back({item.officialRating, ChipStyle::Outline});
    if (item.isSeries()) {
        if (item.childCount > 0)
            out.push_back({item.childCount == 1 ? "1 Season" : std::format("{} Seasons", item.childCount), ChipStyle::Text});
    } else if (auto rt = formatRuntime(item.runTimeTicks); !rt.empty()) {
        out.push_back({rt, ChipStyle::Text});
    }
    if (item.communityRating > 0) out.push_back({formatRating(item.communityRating), ChipStyle::Text, gfx::Icon::Star});
    return out;
}

std::vector<Chip> mediaChips(const jf::MediaSource& source) {
    std::vector<Chip> out;
    if (const jf::MediaStream* v = source.firstOfType(jf::StreamType::Video)) {
        const int w = v->width, h = v->height;
        if (w >= 3200 || h >= 1700) out.push_back({"4K", ChipStyle::Accent});
        else if (w >= 1900 || h >= 1000) out.push_back({"1080p", ChipStyle::Outline});
        else if (w >= 1260 || h >= 700) out.push_back({"720p", ChipStyle::Outline});
        else if (w > 0) out.push_back({"SD", ChipStyle::Outline});
        const std::string range = v->videoRangeType;
        if (range.rfind("DOVI", 0) == 0) out.push_back({"Dolby Vision", ChipStyle::Accent});
        else if (range == "HDR10Plus") out.push_back({"HDR10+", ChipStyle::Accent});
        else if (range == "HDR10") out.push_back({"HDR10", ChipStyle::Accent});
        else if (range == "HLG") out.push_back({"HLG", ChipStyle::Accent});
        else if (v->videoRange == "HDR") out.push_back({"HDR", ChipStyle::Accent});
        const std::string c = lowerAscii(v->codec);
        if (c == "hevc" || c == "h265") out.push_back({v->bitDepth >= 10 ? "HEVC 10-bit" : "HEVC", ChipStyle::Filled});
        else if (c == "h264") out.push_back({"H.264", ChipStyle::Filled});
        else if (c == "av1") out.push_back({"AV1", ChipStyle::Filled});
        else if (c == "vp9") out.push_back({"VP9", ChipStyle::Filled});
        else if (!c.empty()) out.push_back({upperAscii(c), ChipStyle::Filled});
    }
    const jf::MediaStream* a = source.defaultAudioStreamIndex >= 0 ? source.stream(source.defaultAudioStreamIndex) : nullptr;
    if (!a || a->type != jf::StreamType::Audio) a = source.firstOfType(jf::StreamType::Audio);
    if (a) {
        out.push_back({audioCodecName(*a), ChipStyle::Filled});
        if (auto ch = channelsText(a->channels); !ch.empty()) out.push_back({ch, ChipStyle::Filled});
    }
    if (source.firstOfType(jf::StreamType::Subtitle)) out.push_back({"CC", ChipStyle::Outline, gfx::Icon::Subtitles});
    return out;
}

std::string genresText(const jf::BaseItem& item, size_t max) {
    std::string out;
    for (size_t i = 0; i < item.genres.size() && i < max; ++i) {
        if (!out.empty()) out += ", ";
        out += item.genres[i];
    }
    return out;
}

std::string streamTitle(const jf::MediaStream& s) {
    std::string base = s.language.empty() ? std::string() : languageName(s.language);
    if (base.empty() || base == "Unknown") base = !s.title.empty() ? s.title : (!s.displayTitle.empty() ? s.displayTitle : "Track " + std::to_string(s.index));
    if (s.isForced) base += " (Forced)";
    if (s.isHearingImpaired) base += " (SDH)";
    return base;
}

std::string streamDetail(const jf::MediaStream& s) {
    if (s.type == jf::StreamType::Audio) {
        return joinDots({audioCodecName(s), channelsText(s.channels), s.isDefault ? "Default" : "", s.isExternal ? "External" : ""});
    }
    if (s.type == jf::StreamType::Subtitle) {
        std::string fmt = upperAscii(s.subtitleFormat());
        if (fmt.empty()) fmt = upperAscii(s.codec);
        return joinDots({fmt, s.isExternal ? "External" : "Embedded", s.isDefault ? "Default" : "",
                         (!s.title.empty() && !s.language.empty()) ? s.title : ""});
    }
    return s.displayTitle;
}

}  // namespace karafin::screens
