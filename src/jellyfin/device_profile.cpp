#include "jellyfin/device_profile.hpp"

#include <algorithm>
#include <string>

#include "core/settings.hpp"

namespace karafin::jellyfin {

using Json = nlohmann::json;

DeviceProfileOptions DeviceProfileOptions::fromSettings() {
    DeviceProfileOptions o;
    auto& s = Settings::instance();
    o.maxStreamingBitrate = s.get<int64_t>("jellyfin.max_streaming_bitrate", o.maxStreamingBitrate);
    o.maxAudioChannels = s.get<int>("jellyfin.max_audio_channels", o.maxAudioChannels);
    o.enableAv1 = s.get<bool>("jellyfin.enable_av1", o.enableAv1);
    o.maxStreamingBitrate = std::clamp<int64_t>(o.maxStreamingBitrate, 500'000, kMaxProfileBitrate);
    if (o.maxStaticBitrate < o.maxStreamingBitrate) o.maxStaticBitrate = o.maxStreamingBitrate;
    if (o.maxAudioChannels < 2) o.maxAudioChannels = 2;
    return o;
}

namespace {

Json condition(const char* cond, const char* property, const std::string& value, bool required = false) {
    return Json{{"Condition", cond}, {"Property", property}, {"Value", value}, {"IsRequired", required}};
}

}  // namespace

Json buildDeviceProfile(const DeviceProfileOptions& o) {
    const std::string containers = "mkv,matroska,webm,mp4,m4v,mov,avi,ts,mpegts,m2ts";
    std::string videoCodecs = "h264,hevc,vp9,vp8,mpeg2video,mpeg4,vc1";
    if (o.enableAv1) videoCodecs += ",av1";
    const std::string audioCodecs =
        "aac,ac3,eac3,flac,mp3,mp2,opus,vorbis,dts,dca,truehd,alac,pcm,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le";
    const std::string channels = std::to_string(o.maxAudioChannels);

    Json directPlay = Json::array();
    directPlay.push_back(
        {{"Container", containers}, {"Type", "Video"}, {"VideoCodec", videoCodecs}, {"AudioCodec", audioCodecs}});
    directPlay.push_back({{"Container", "mp3,flac,aac,m4a,ogg,opus,wav"}, {"Type", "Audio"}});

    Json transcoding = Json::array();
    transcoding.push_back({{"Container", "ts"},
                           {"Type", "Video"},
                           {"VideoCodec", "h264"},
                           {"AudioCodec", "aac,mp3,ac3"},
                           {"Protocol", "hls"},
                           {"Context", "Streaming"},
                           {"MaxAudioChannels", channels},
                           {"MinSegments", 1},
                           {"SegmentLength", 6},
                           {"BreakOnNonKeyFrames", true},
                           {"CopyTimestamps", false},
                           {"EnableSubtitlesInManifest", false}});
    transcoding.push_back({{"Container", "mp3"},
                           {"Type", "Audio"},
                           {"AudioCodec", "mp3"},
                           {"Protocol", "http"},
                           {"Context", "Streaming"},
                           {"MaxAudioChannels", "2"}});

    Json codecProfiles = Json::array();
    // H.264: nvtegra handles 8-bit up to level 5.2; Hi10P must be transcoded.
    codecProfiles.push_back({{"Type", "Video"},
                             {"Codec", "h264"},
                             {"Conditions", Json::array({condition("LessThanEqual", "VideoLevel", "52"),
                                                         condition("LessThanEqual", "VideoBitDepth", "8"),
                                                         condition("LessThanEqual", "Width", "3840"),
                                                         condition("LessThanEqual", "Height", "2160")})}});
    // HEVC: Main / Main10 up to level 5.1 (153). HDR is direct played (mpv tone-maps); Dolby
    // Vision is fine when it carries an HDR10/HLG/SDR base layer (the EL / RPU is ignored), but
    // profile 5 (DOVI, no fallback) would show wrong colours, so it is transcoded. Range names the
    // server doesn't know (10.11 additions on older servers) are skipped by its parser.
    codecProfiles.push_back(
        {{"Type", "Video"},
         {"Codec", "hevc"},
         {"Conditions",
          Json::array({condition("LessThanEqual", "VideoLevel", "153"),
                       condition("LessThanEqual", "VideoBitDepth", "10"),
                       condition("LessThanEqual", "Width", "3840"), condition("LessThanEqual", "Height", "2160"),
                       condition("EqualsAny", "VideoProfile", "main|main 10"),
                       condition("EqualsAny", "VideoRangeType",
                                 "SDR|HDR10|HDR10Plus|HLG|DOVIWithHDR10|DOVIWithHLG|DOVIWithSDR|DOVIWithEL|"
                                 "DOVIWithHDR10Plus|DOVIWithELHDR10Plus")})}});
    codecProfiles.push_back({{"Type", "Video"},
                             {"Codec", "vp9"},
                             {"Conditions", Json::array({condition("LessThanEqual", "Width", "3840"),
                                                         condition("LessThanEqual", "Height", "2160"),
                                                         condition("LessThanEqual", "VideoBitDepth", "10")})}});
    if (o.enableAv1) {
        // AV1 decodes in software (dav1d): cap the resolution so it stays real-time.
        codecProfiles.push_back(
            {{"Type", "Video"},
             {"Codec", "av1"},
             {"Conditions", Json::array({condition("LessThanEqual", "Width", std::to_string(o.av1MaxWidth)),
                                         condition("LessThanEqual", "Height", std::to_string(o.av1MaxHeight)),
                                         condition("LessThanEqual", "VideoBitDepth", "10"),
                                         condition("LessThanEqual", "VideoFramerate", "60")})}});
    }
    codecProfiles.push_back({{"Type", "Video"},
                             {"Codec", "mpeg2video,vc1,mpeg4"},
                             {"Conditions", Json::array({condition("LessThanEqual", "Width", "1920"),
                                                         condition("LessThanEqual", "Height", "1080")})}});
    codecProfiles.push_back(
        {{"Type", "VideoAudio"},
         {"Conditions", Json::array({condition("LessThanEqual", "AudioChannels", channels)})}});

    Json subtitles = Json::array();
    for (const char* fmt : {"srt", "subrip", "ass", "ssa", "vtt", "webvtt", "mov_text", "pgssub", "pgs", "dvdsub",
                            "dvbsub", "sub", "idx"})
        subtitles.push_back({{"Format", fmt}, {"Method", "Embed"}});
    for (const char* fmt : {"srt", "subrip", "ass", "ssa", "vtt", "webvtt"})
        subtitles.push_back({{"Format", fmt}, {"Method", "External"}});

    return Json{
        {"Name", "Karafin (Nintendo Switch)"},
        {"MaxStreamingBitrate", o.maxStreamingBitrate},
        {"MaxStaticBitrate", o.maxStaticBitrate},
        {"MaxStaticMusicBitrate", 3'000'000},
        {"MusicStreamingTranscodingBitrate", 320'000},
        {"DirectPlayProfiles", directPlay},
        {"TranscodingProfiles", transcoding},
        {"ContainerProfiles", Json::array()},
        {"CodecProfiles", codecProfiles},
        {"SubtitleProfiles", subtitles},
        {"ResponseProfiles", Json::array()},
    };
}

}  // namespace karafin::jellyfin
