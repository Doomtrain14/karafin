#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Jellyfin DTOs (subset). Every parse* function tolerates missing fields, nulls and wrong types:
// unknown/invalid values fall back to the defaults below. Ticks are 100 ns units (10'000'000/s).
namespace karafin::jellyfin {

using Json = nlohmann::json;

inline constexpr int64_t kTicksPerSecond = 10'000'000;
inline constexpr int64_t kTicksPerMs = 10'000;
inline double ticksToSeconds(int64_t ticks) { return static_cast<double>(ticks) / kTicksPerSecond; }
inline int64_t secondsToTicks(double seconds) { return static_cast<int64_t>(seconds * kTicksPerSecond); }

struct ServerInfo {
    std::string id;
    std::string name;
    std::string version;        // "10.9.11"
    std::string productName;    // "Jellyfin Server"
    std::string localAddress;
    std::string operatingSystem;
    bool startupWizardCompleted = true;
};

struct UserPolicy {
    bool isAdministrator = false;
    bool isDisabled = false;
    bool enableContentDownloading = true;  // "Allow media downloading"
    bool enableSubtitleManagement = false;  // needed for remote subtitle download (admins always can)
    bool enableMediaPlayback = true;
    bool enableVideoPlaybackTranscoding = true;
    bool enableRemoteAccess = true;
};

struct UserDto {
    std::string id;
    std::string name;
    std::string serverId;
    std::string primaryImageTag;
    bool hasPassword = true;
    bool hasConfiguredPassword = true;
    std::string lastLoginDate;
    std::string lastActivityDate;
    UserPolicy policy;

    bool canDownload() const { return policy.isAdministrator || policy.enableContentDownloading; }
    bool canManageSubtitles() const { return policy.isAdministrator || policy.enableSubtitleManagement; }
};

struct AuthResult {
    UserDto user;
    std::string accessToken;
    std::string serverId;
};

struct QuickConnectState {
    std::string secret;
    std::string code;           // 6-digit code the user enters in another client
    bool authenticated = false;
    std::string dateAdded;
};

struct UserData {
    int64_t playbackPositionTicks = 0;
    bool played = false;
    bool isFavorite = false;
    double playedPercentage = 0;  // 0 when unknown
    int unplayedItemCount = 0;
    int playCount = 0;
    std::string lastPlayedDate;
    std::string key;
};

enum class StreamType { Unknown, Video, Audio, Subtitle, EmbeddedImage, Data, Lyric };
enum class SubtitleDeliveryMethod { Unknown, Encode, Embed, External, Hls, Drop };

StreamType parseStreamType(std::string_view s);
std::string_view streamTypeName(StreamType t);
SubtitleDeliveryMethod parseDeliveryMethod(std::string_view s);
std::string_view deliveryMethodName(SubtitleDeliveryMethod m);

struct MediaStream {
    StreamType type = StreamType::Unknown;
    int index = -1;
    std::string codec;          // "h264", "hevc", "aac", "subrip", "ass", "pgssub", ...
    std::string codecTag;
    std::string language;       // ISO 639-2 ("eng"), may be empty
    std::string title;
    std::string displayTitle;   // "English - SRT - Default"
    std::string profile;
    bool isExternal = false;
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
    bool isTextSubtitleStream = false;
    bool supportsExternalStream = false;
    bool isInterlaced = false;
    SubtitleDeliveryMethod deliveryMethod = SubtitleDeliveryMethod::Unknown;
    std::string deliveryUrl;    // server-relative, e.g. "/Videos/.../Subtitles/3/0/Stream.srt?api_key=..."
    std::string path;           // external file path on the server
    // video
    int width = 0;
    int height = 0;
    int bitDepth = 0;
    double level = 0;
    double averageFrameRate = 0;
    double realFrameRate = 0;
    std::string videoRange;      // "SDR", "HDR"
    std::string videoRangeType;  // "SDR", "HDR10", "DOVI", "HLG", ...
    std::string aspectRatio;
    // audio
    int channels = 0;
    std::string channelLayout;
    int sampleRate = 0;
    int64_t bitRate = 0;

    bool isSubtitle() const { return type == StreamType::Subtitle; }
    // Extension Jellyfin can serve this subtitle as ("srt", "ass", "ssa", "vtt", "sup", ...).
    std::string subtitleFormat() const;
};

struct MediaSource {
    std::string id;
    std::string name;
    std::string path;
    std::string protocol;        // "File", "Http", ...
    std::string container;       // "mkv", "mp4", "mov,mp4,m4a,3gp,3g2,mj2", ...
    std::string eTag;
    std::string type;            // "Default", "Grouping", "Placeholder"
    int64_t size = 0;
    int64_t runTimeTicks = 0;
    int64_t bitrate = 0;
    bool isRemote = false;
    bool supportsDirectPlay = false;
    bool supportsDirectStream = false;
    bool supportsTranscoding = false;
    bool requiresOpening = false;
    std::string transcodingUrl;  // server-relative (HLS master playlist) when transcoding is offered
    std::string transcodingSubProtocol;  // "hls" / "http"
    std::string transcodingContainer;
    std::string liveStreamId;
    std::vector<MediaStream> mediaStreams;
    int defaultAudioStreamIndex = -1;
    int defaultSubtitleStreamIndex = -1;  // -1: none / off

    const MediaStream* stream(int index) const;
    const MediaStream* firstOfType(StreamType t) const;
    std::vector<const MediaStream*> streamsOfType(StreamType t) const;
};

struct ChapterInfo {
    int64_t startPositionTicks = 0;
    std::string name;
    std::string imageTag;
};

struct PersonInfo {
    std::string id;
    std::string name;
    std::string role;       // character name for actors
    std::string type;       // "Actor", "Director", "Writer", ...
    std::string primaryImageTag;
};

struct NameIdPair {
    std::string name;
    std::string id;
};

// One trickplay resolution of one media source. Thumbnails are packed tileWidth x tileHeight per
// sheet image (/Videos/{id}/Trickplay/{width}/{sheet}.jpg), one thumbnail every `intervalMs`.
struct TrickplayInfo {
    int width = 0;
    int height = 0;
    int tileWidth = 0;
    int tileHeight = 0;
    int thumbnailCount = 0;
    int intervalMs = 0;
    int bandwidth = 0;

    struct Tile {
        int sheetIndex = 0;  // which .jpg
        int x = 0, y = 0;    // pixel offset of the thumbnail inside the sheet
        int w = 0, h = 0;
    };
    bool valid() const { return width > 0 && height > 0 && tileWidth > 0 && tileHeight > 0 && intervalMs > 0; }
    int sheetCount() const;
    Tile tileAt(int64_t positionMs) const;
};

// mediaSourceId -> (width -> info)
using TrickplayMap = std::map<std::string, std::map<int, TrickplayInfo>>;

struct BaseItem {
    std::string id;
    std::string name;
    std::string originalTitle;
    std::string sortName;
    std::string type;            // "Movie", "Series", "Season", "Episode", "CollectionFolder", "BoxSet", ...
    std::string mediaType;       // "Video", "Audio", ...
    std::string collectionType;  // views: "movies", "tvshows", "boxsets", "music", ...
    std::string serverId;
    std::string parentId;
    std::string seriesId;
    std::string seriesName;
    std::string seasonId;
    std::string seasonName;
    int indexNumber = -1;        // episode / season number, -1 when unknown
    int indexNumberEnd = -1;
    int parentIndexNumber = -1;  // season number of an episode
    std::string overview;
    std::vector<std::string> taglines;
    int productionYear = 0;
    std::string premiereDate;
    std::string endDate;
    std::string dateCreated;
    std::string officialRating;  // "PG-13"
    double communityRating = 0;
    double criticRating = 0;
    int64_t runTimeTicks = 0;
    std::string status;          // series: "Continuing" / "Ended"
    bool isFolder = false;
    int childCount = 0;
    int recursiveItemCount = 0;
    double primaryImageAspectRatio = 0;
    UserData userData;

    std::map<std::string, std::string> imageTags;  // "Primary" -> tag, "Logo" -> tag, ...
    std::vector<std::string> backdropImageTags;
    std::string parentBackdropItemId;
    std::vector<std::string> parentBackdropImageTags;
    std::string parentThumbItemId;
    std::string parentThumbImageTag;
    std::string parentLogoItemId;
    std::string parentLogoImageTag;
    std::string parentPrimaryImageItemId;
    std::string parentPrimaryImageTag;
    std::string seriesPrimaryImageTag;
    std::string seriesThumbImageTag;

    std::vector<std::string> genres;
    std::vector<std::string> tags;
    std::vector<PersonInfo> people;
    std::vector<NameIdPair> studios;
    std::vector<MediaStream> mediaStreams;
    std::vector<MediaSource> mediaSources;
    std::vector<ChapterInfo> chapters;
    TrickplayMap trickplay;
    std::map<std::string, std::string> providerIds;  // "Imdb" -> "tt..."

    bool canDownload = false;
    std::string path;
    std::string container;
    int64_t size = 0;            // DTO "Size" or, if absent, the first media source's size
    std::string locationType;    // "FileSystem", "Virtual" (missing episode), ...

    bool isEpisode() const { return type == "Episode"; }
    bool isSeries() const { return type == "Series"; }
    bool isSeason() const { return type == "Season"; }
    bool isMovie() const { return type == "Movie"; }
    bool isVirtual() const { return locationType == "Virtual"; }
    bool isPlayable() const;
    std::string imageTag(std::string_view imageType) const;  // "" if none
    double runTimeSeconds() const { return ticksToSeconds(runTimeTicks); }
    double resumeSeconds() const { return ticksToSeconds(userData.playbackPositionTicks); }
    // 0..1 from UserData.PlayedPercentage or position/runtime.
    double progress() const;
    // "S1:E3", "S1:E3-4", "E3", or "" when not an episode.
    std::string episodeTag() const;
    const MediaSource* mediaSource(std::string_view id) const;  // id "" -> first
};

struct MediaSegment {
    enum class Type { Unknown, Intro, Outro, Recap, Preview, Commercial };
    std::string id;
    std::string itemId;
    Type type = Type::Unknown;
    int64_t startTicks = 0;
    int64_t endTicks = 0;

    double startSeconds() const { return ticksToSeconds(startTicks); }
    double endSeconds() const { return ticksToSeconds(endTicks); }
    bool contains(int64_t ticks) const { return ticks >= startTicks && ticks < endTicks; }
};
std::string_view segmentTypeName(MediaSegment::Type t);  // "Intro", "Outro" (credits), ...

struct RemoteSubtitleInfo {
    std::string id;              // pass to Client::downloadRemoteSubtitle
    std::string providerName;    // "Open Subtitles"
    std::string name;            // release name
    std::string format;          // "srt", "ass", ...
    std::string language;        // ThreeLetterISOLanguageName
    std::string author;
    std::string comment;
    std::string dateCreated;
    double communityRating = 0;
    double frameRate = 0;
    int downloadCount = 0;
    bool isHashMatch = false;
    bool forced = false;
    bool hearingImpaired = false;
    bool aiTranslated = false;
    bool machineTranslated = false;
};

struct SearchHint {
    std::string id;
    std::string name;
    std::string type;
    std::string mediaType;
    int productionYear = 0;
    int indexNumber = -1;
    int parentIndexNumber = -1;
    std::string series;
    std::string primaryImageTag;
    std::string thumbImageTag;
    std::string thumbImageItemId;
    std::string backdropImageTag;
    std::string backdropImageItemId;
    int64_t runTimeTicks = 0;
    double primaryImageAspectRatio = 0;
};

template <class T>
struct QueryResult {
    std::vector<T> items;
    int totalRecordCount = 0;
    int startIndex = 0;
};

// Server discovery reply (UDP 7359).
struct DiscoveredServer {
    std::string id;
    std::string name;
    std::string address;         // "http://192.168.1.10:8096" as reported by the server
    std::string endpointAddress;
    std::string senderIp;        // where the reply came from
};

// --- Parsers -----------------------------------------------------------------------------------

ServerInfo parseServerInfo(const Json& j);
UserPolicy parseUserPolicy(const Json& j);
UserDto parseUser(const Json& j);
AuthResult parseAuthResult(const Json& j);
QuickConnectState parseQuickConnect(const Json& j);
UserData parseUserData(const Json& j);
MediaStream parseMediaStream(const Json& j);
MediaSource parseMediaSource(const Json& j);
ChapterInfo parseChapter(const Json& j);
PersonInfo parsePerson(const Json& j);
TrickplayInfo parseTrickplayInfo(const Json& j);
TrickplayMap parseTrickplayMap(const Json& j);
BaseItem parseItem(const Json& j);
MediaSegment parseMediaSegment(const Json& j);
RemoteSubtitleInfo parseRemoteSubtitle(const Json& j);
SearchHint parseSearchHint(const Json& j);
DiscoveredServer parseDiscoveryReply(const Json& j);

// {"Items":[...],"TotalRecordCount":N,"StartIndex":S}; a bare array is accepted too.
template <class T, class F>
QueryResult<T> parseQueryResult(const Json& j, F&& parseOne) {
    QueryResult<T> out;
    const Json* arr = nullptr;
    if (j.is_array()) {
        arr = &j;
    } else if (j.is_object()) {
        auto it = j.find("Items");
        if (it != j.end() && it->is_array()) arr = &*it;
        auto total = j.find("TotalRecordCount");
        if (total != j.end() && total->is_number()) out.totalRecordCount = total->get<int>();
        auto start = j.find("StartIndex");
        if (start != j.end() && start->is_number()) out.startIndex = start->get<int>();
    }
    if (arr) {
        out.items.reserve(arr->size());
        for (const auto& e : *arr)
            if (e.is_object()) out.items.push_back(parseOne(e));
    }
    if (out.totalRecordCount < static_cast<int>(out.items.size()))
        out.totalRecordCount = out.startIndex + static_cast<int>(out.items.size());
    return out;
}

QueryResult<BaseItem> parseItems(const Json& j);

// Version helpers: "10.9.11" -> {10, 9, 11}. compareVersions returns <0, 0, >0.
int compareVersions(std::string_view a, std::string_view b);

}  // namespace karafin::jellyfin
