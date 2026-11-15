#include "jellyfin/models.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "jellyfin/detail/json_get.hpp"

namespace karafin::jellyfin {

using namespace detail;

// --- enums -------------------------------------------------------------------------------------

StreamType parseStreamType(std::string_view s) {
    if (s == "Video") return StreamType::Video;
    if (s == "Audio") return StreamType::Audio;
    if (s == "Subtitle") return StreamType::Subtitle;
    if (s == "EmbeddedImage") return StreamType::EmbeddedImage;
    if (s == "Data") return StreamType::Data;
    if (s == "Lyric") return StreamType::Lyric;
    return StreamType::Unknown;
}

std::string_view streamTypeName(StreamType t) {
    switch (t) {
        case StreamType::Video: return "Video";
        case StreamType::Audio: return "Audio";
        case StreamType::Subtitle: return "Subtitle";
        case StreamType::EmbeddedImage: return "EmbeddedImage";
        case StreamType::Data: return "Data";
        case StreamType::Lyric: return "Lyric";
        case StreamType::Unknown: break;
    }
    return "Unknown";
}

SubtitleDeliveryMethod parseDeliveryMethod(std::string_view s) {
    if (s == "Encode") return SubtitleDeliveryMethod::Encode;
    if (s == "Embed") return SubtitleDeliveryMethod::Embed;
    if (s == "External") return SubtitleDeliveryMethod::External;
    if (s == "Hls") return SubtitleDeliveryMethod::Hls;
    if (s == "Drop") return SubtitleDeliveryMethod::Drop;
    return SubtitleDeliveryMethod::Unknown;
}

std::string_view deliveryMethodName(SubtitleDeliveryMethod m) {
    switch (m) {
        case SubtitleDeliveryMethod::Encode: return "Encode";
        case SubtitleDeliveryMethod::Embed: return "Embed";
        case SubtitleDeliveryMethod::External: return "External";
        case SubtitleDeliveryMethod::Hls: return "Hls";
        case SubtitleDeliveryMethod::Drop: return "Drop";
        case SubtitleDeliveryMethod::Unknown: break;
    }
    return "Unknown";
}

std::string_view segmentTypeName(MediaSegment::Type t) {
    switch (t) {
        case MediaSegment::Type::Intro: return "Intro";
        case MediaSegment::Type::Outro: return "Outro";
        case MediaSegment::Type::Recap: return "Recap";
        case MediaSegment::Type::Preview: return "Preview";
        case MediaSegment::Type::Commercial: return "Commercial";
        case MediaSegment::Type::Unknown: break;
    }
    return "Unknown";
}

// --- helpers -----------------------------------------------------------------------------------

std::string MediaStream::subtitleFormat() const {
    std::string c = codec;
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (c == "subrip" || c == "srt") return "srt";
    if (c == "ass") return "ass";
    if (c == "ssa") return "ssa";
    if (c == "webvtt" || c == "vtt") return "vtt";
    if (c == "pgssub" || c == "pgs" || c == "sup") return "sup";
    if (c == "mov_text") return "srt";
    if (c == "ttml") return "ttml";
    if (c.empty()) return "srt";
    return c;
}

const MediaStream* MediaSource::stream(int index) const {
    for (const auto& s : mediaStreams)
        if (s.index == index) return &s;
    return nullptr;
}

const MediaStream* MediaSource::firstOfType(StreamType t) const {
    for (const auto& s : mediaStreams)
        if (s.type == t) return &s;
    return nullptr;
}

std::vector<const MediaStream*> MediaSource::streamsOfType(StreamType t) const {
    std::vector<const MediaStream*> out;
    for (const auto& s : mediaStreams)
        if (s.type == t) out.push_back(&s);
    return out;
}

int TrickplayInfo::sheetCount() const {
    const int perSheet = tileWidth * tileHeight;
    if (perSheet <= 0 || thumbnailCount <= 0) return 0;
    return (thumbnailCount + perSheet - 1) / perSheet;
}

TrickplayInfo::Tile TrickplayInfo::tileAt(int64_t positionMs) const {
    Tile t;
    if (!valid()) return t;
    int64_t thumb = positionMs <= 0 ? 0 : positionMs / intervalMs;
    if (thumbnailCount > 0 && thumb >= thumbnailCount) thumb = thumbnailCount - 1;
    const int perSheet = tileWidth * tileHeight;
    const int idx = static_cast<int>(thumb);
    t.sheetIndex = idx / perSheet;
    const int inSheet = idx % perSheet;
    t.x = (inSheet % tileWidth) * width;
    t.y = (inSheet / tileWidth) * height;
    t.w = width;
    t.h = height;
    return t;
}

bool BaseItem::isPlayable() const {
    if (isFolder || isVirtual()) return false;
    return mediaType == "Video" || type == "Movie" || type == "Episode" || type == "Video" ||
           type == "MusicVideo" || type == "Trailer";
}

std::string BaseItem::imageTag(std::string_view imageType) const {
    auto it = imageTags.find(std::string(imageType));
    return it == imageTags.end() ? std::string() : it->second;
}

double BaseItem::progress() const {
    if (userData.playedPercentage > 0) return std::clamp(userData.playedPercentage / 100.0, 0.0, 1.0);
    if (runTimeTicks > 0 && userData.playbackPositionTicks > 0)
        return std::clamp(static_cast<double>(userData.playbackPositionTicks) / static_cast<double>(runTimeTicks), 0.0,
                          1.0);
    return 0;
}

std::string BaseItem::episodeTag() const {
    if (!isEpisode()) return {};
    std::string out;
    if (parentIndexNumber >= 0) out += "S" + std::to_string(parentIndexNumber) + ":";
    if (indexNumber >= 0) {
        out += "E" + std::to_string(indexNumber);
        if (indexNumberEnd > indexNumber) out += "-" + std::to_string(indexNumberEnd);
    }
    if (!out.empty() && out.back() == ':') out.pop_back();
    return out;
}

const MediaSource* BaseItem::mediaSource(std::string_view sourceId) const {
    if (mediaSources.empty()) return nullptr;
    if (sourceId.empty()) return &mediaSources.front();
    for (const auto& ms : mediaSources)
        if (ms.id == sourceId) return &ms;
    return nullptr;
}

// --- parsers -----------------------------------------------------------------------------------

ServerInfo parseServerInfo(const Json& j) {
    ServerInfo s;
    s.id = getStr(j, "Id");
    s.name = getStr(j, "ServerName");
    s.version = getStr(j, "Version");
    s.productName = getStr(j, "ProductName");
    s.localAddress = getStr(j, "LocalAddress");
    s.operatingSystem = getStr(j, "OperatingSystem");
    s.startupWizardCompleted = getBool(j, "StartupWizardCompleted", true);
    return s;
}

UserPolicy parseUserPolicy(const Json& j) {
    UserPolicy p;
    p.isAdministrator = getBool(j, "IsAdministrator");
    p.isDisabled = getBool(j, "IsDisabled");
    p.enableContentDownloading = getBool(j, "EnableContentDownloading", true);
    p.enableSubtitleManagement = getBool(j, "EnableSubtitleManagement", false);
    p.enableMediaPlayback = getBool(j, "EnableMediaPlayback", true);
    p.enableVideoPlaybackTranscoding = getBool(j, "EnableVideoPlaybackTranscoding", true);
    p.enableRemoteAccess = getBool(j, "EnableRemoteAccess", true);
    return p;
}

UserDto parseUser(const Json& j) {
    UserDto u;
    u.id = getStr(j, "Id");
    u.name = getStr(j, "Name");
    u.serverId = getStr(j, "ServerId");
    u.primaryImageTag = getStr(j, "PrimaryImageTag");
    u.hasPassword = getBool(j, "HasPassword", true);
    u.hasConfiguredPassword = getBool(j, "HasConfiguredPassword", u.hasPassword);
    u.lastLoginDate = getStr(j, "LastLoginDate");
    u.lastActivityDate = getStr(j, "LastActivityDate");
    u.policy = parseUserPolicy(getObj(j, "Policy"));
    return u;
}

AuthResult parseAuthResult(const Json& j) {
    AuthResult a;
    a.user = parseUser(getObj(j, "User"));
    a.accessToken = getStr(j, "AccessToken");
    a.serverId = getStr(j, "ServerId", a.user.serverId);
    return a;
}

QuickConnectState parseQuickConnect(const Json& j) {
    QuickConnectState q;
    q.secret = getStr(j, "Secret");
    q.code = getStr(j, "Code");
    q.authenticated = getBool(j, "Authenticated");
    q.dateAdded = getStr(j, "DateAdded");
    return q;
}

UserData parseUserData(const Json& j) {
    UserData d;
    d.playbackPositionTicks = getI64(j, "PlaybackPositionTicks");
    d.played = getBool(j, "Played");
    d.isFavorite = getBool(j, "IsFavorite");
    d.playedPercentage = getDouble(j, "PlayedPercentage");
    d.unplayedItemCount = getInt(j, "UnplayedItemCount");
    d.playCount = getInt(j, "PlayCount");
    d.lastPlayedDate = getStr(j, "LastPlayedDate");
    d.key = getStr(j, "Key");
    return d;
}

MediaStream parseMediaStream(const Json& j) {
    MediaStream s;
    s.type = parseStreamType(getStr(j, "Type"));
    s.index = getInt(j, "Index", -1);
    s.codec = getStr(j, "Codec");
    s.codecTag = getStr(j, "CodecTag");
    s.language = getStr(j, "Language");
    s.title = getStr(j, "Title");
    s.displayTitle = getStr(j, "DisplayTitle");
    s.profile = getStr(j, "Profile");
    s.isExternal = getBool(j, "IsExternal");
    s.isDefault = getBool(j, "IsDefault");
    s.isForced = getBool(j, "IsForced");
    s.isHearingImpaired = getBool(j, "IsHearingImpaired");
    s.isTextSubtitleStream = getBool(j, "IsTextSubtitleStream");
    s.supportsExternalStream = getBool(j, "SupportsExternalStream");
    s.isInterlaced = getBool(j, "IsInterlaced");
    s.deliveryMethod = parseDeliveryMethod(getStr(j, "DeliveryMethod"));
    s.deliveryUrl = getStr(j, "DeliveryUrl");
    s.path = getStr(j, "Path");
    s.width = getInt(j, "Width");
    s.height = getInt(j, "Height");
    s.bitDepth = getInt(j, "BitDepth");
    s.level = getDouble(j, "Level");
    s.averageFrameRate = getDouble(j, "AverageFrameRate");
    s.realFrameRate = getDouble(j, "RealFrameRate");
    s.videoRange = getStr(j, "VideoRange");
    s.videoRangeType = getStr(j, "VideoRangeType");
    s.aspectRatio = getStr(j, "AspectRatio");
    s.channels = getInt(j, "Channels");
    s.channelLayout = getStr(j, "ChannelLayout");
    s.sampleRate = getInt(j, "SampleRate");
    s.bitRate = getI64(j, "BitRate");
    if (s.displayTitle.empty()) s.displayTitle = !s.title.empty() ? s.title : s.language;
    return s;
}

MediaSource parseMediaSource(const Json& j) {
    MediaSource m;
    m.id = getStr(j, "Id");
    m.name = getStr(j, "Name");
    m.path = getStr(j, "Path");
    m.protocol = getStr(j, "Protocol");
    m.container = getStr(j, "Container");
    m.eTag = getStr(j, "ETag");
    m.type = getStr(j, "Type");
    m.size = getI64(j, "Size");
    m.runTimeTicks = getI64(j, "RunTimeTicks");
    m.bitrate = getI64(j, "Bitrate");
    m.isRemote = getBool(j, "IsRemote");
    m.supportsDirectPlay = getBool(j, "SupportsDirectPlay");
    m.supportsDirectStream = getBool(j, "SupportsDirectStream");
    m.supportsTranscoding = getBool(j, "SupportsTranscoding");
    m.requiresOpening = getBool(j, "RequiresOpening");
    m.transcodingUrl = getStr(j, "TranscodingUrl");
    m.transcodingSubProtocol = getStr(j, "TranscodingSubProtocol");
    m.transcodingContainer = getStr(j, "TranscodingContainer");
    m.liveStreamId = getStr(j, "LiveStreamId");
    for (const auto& s : getArr(j, "MediaStreams"))
        if (s.is_object()) m.mediaStreams.push_back(parseMediaStream(s));
    m.defaultAudioStreamIndex = getInt(j, "DefaultAudioStreamIndex", -1);
    m.defaultSubtitleStreamIndex = getInt(j, "DefaultSubtitleStreamIndex", -1);
    return m;
}

ChapterInfo parseChapter(const Json& j) {
    ChapterInfo c;
    c.startPositionTicks = getI64(j, "StartPositionTicks");
    c.name = getStr(j, "Name");
    c.imageTag = getStr(j, "ImageTag");
    return c;
}

PersonInfo parsePerson(const Json& j) {
    PersonInfo p;
    p.id = getStr(j, "Id");
    p.name = getStr(j, "Name");
    p.role = getStr(j, "Role");
    p.type = getStr(j, "Type");
    p.primaryImageTag = getStr(j, "PrimaryImageTag");
    return p;
}

TrickplayInfo parseTrickplayInfo(const Json& j) {
    TrickplayInfo t;
    t.width = getInt(j, "Width");
    t.height = getInt(j, "Height");
    t.tileWidth = getInt(j, "TileWidth");
    t.tileHeight = getInt(j, "TileHeight");
    t.thumbnailCount = getInt(j, "ThumbnailCount");
    t.intervalMs = getInt(j, "Interval");
    t.bandwidth = getInt(j, "Bandwidth");
    return t;
}

TrickplayMap parseTrickplayMap(const Json& j) {
    TrickplayMap out;
    if (!j.is_object()) return out;
    for (auto src = j.begin(); src != j.end(); ++src) {
        if (!src->is_object()) continue;
        auto& widths = out[src.key()];
        for (auto w = src->begin(); w != src->end(); ++w) {
            if (!w->is_object()) continue;
            TrickplayInfo info = parseTrickplayInfo(*w);
            if (info.width <= 0) {
                int parsed = 0;
                const std::string& k = w.key();
                std::from_chars(k.data(), k.data() + k.size(), parsed);
                info.width = parsed;
            }
            if (info.width > 0) widths[info.width] = info;
        }
    }
    return out;
}

BaseItem parseItem(const Json& j) {
    BaseItem it;
    it.id = getStr(j, "Id");
    it.name = getStr(j, "Name");
    it.originalTitle = getStr(j, "OriginalTitle");
    it.sortName = getStr(j, "SortName");
    it.type = getStr(j, "Type");
    it.mediaType = getStr(j, "MediaType");
    it.collectionType = getStr(j, "CollectionType");
    it.serverId = getStr(j, "ServerId");
    it.parentId = getStr(j, "ParentId");
    it.seriesId = getStr(j, "SeriesId");
    it.seriesName = getStr(j, "SeriesName");
    it.seasonId = getStr(j, "SeasonId");
    it.seasonName = getStr(j, "SeasonName");
    it.indexNumber = getInt(j, "IndexNumber", -1);
    it.indexNumberEnd = getInt(j, "IndexNumberEnd", -1);
    it.parentIndexNumber = getInt(j, "ParentIndexNumber", -1);
    it.overview = getStr(j, "Overview");
    it.taglines = getStrList(j, "Taglines");
    it.productionYear = getInt(j, "ProductionYear");
    it.premiereDate = getStr(j, "PremiereDate");
    it.endDate = getStr(j, "EndDate");
    it.dateCreated = getStr(j, "DateCreated");
    it.officialRating = getStr(j, "OfficialRating");
    it.communityRating = getDouble(j, "CommunityRating");
    it.criticRating = getDouble(j, "CriticRating");
    it.runTimeTicks = getI64(j, "RunTimeTicks");
    it.status = getStr(j, "Status");
    it.isFolder = getBool(j, "IsFolder");
    it.childCount = getInt(j, "ChildCount");
    it.recursiveItemCount = getInt(j, "RecursiveItemCount");
    it.primaryImageAspectRatio = getDouble(j, "PrimaryImageAspectRatio");
    it.userData = parseUserData(getObj(j, "UserData"));

    const Json& tags = getObj(j, "ImageTags");
    for (auto t = tags.begin(); t != tags.end(); ++t)
        if (t->is_string()) it.imageTags[t.key()] = t->get<std::string>();
    it.backdropImageTags = getStrList(j, "BackdropImageTags");
    it.parentBackdropItemId = getStr(j, "ParentBackdropItemId");
    it.parentBackdropImageTags = getStrList(j, "ParentBackdropImageTags");
    it.parentThumbItemId = getStr(j, "ParentThumbItemId");
    it.parentThumbImageTag = getStr(j, "ParentThumbImageTag");
    it.parentLogoItemId = getStr(j, "ParentLogoItemId");
    it.parentLogoImageTag = getStr(j, "ParentLogoImageTag");
    it.parentPrimaryImageItemId = getStr(j, "ParentPrimaryImageItemId");
    it.parentPrimaryImageTag = getStr(j, "ParentPrimaryImageTag");
    it.seriesPrimaryImageTag = getStr(j, "SeriesPrimaryImageTag");
    it.seriesThumbImageTag = getStr(j, "SeriesThumbImageTag");

    it.genres = getStrList(j, "Genres");
    it.tags = getStrList(j, "Tags");
    for (const auto& p : getArr(j, "People"))
        if (p.is_object()) it.people.push_back(parsePerson(p));
    for (const auto& s : getArr(j, "Studios")) {
        if (s.is_object()) {
            it.studios.push_back({getStr(s, "Name"), getStr(s, "Id")});
        } else if (s.is_string()) {
            it.studios.push_back({s.get<std::string>(), {}});
        }
    }
    for (const auto& s : getArr(j, "MediaStreams"))
        if (s.is_object()) it.mediaStreams.push_back(parseMediaStream(s));
    for (const auto& s : getArr(j, "MediaSources"))
        if (s.is_object()) it.mediaSources.push_back(parseMediaSource(s));
    for (const auto& c : getArr(j, "Chapters"))
        if (c.is_object()) it.chapters.push_back(parseChapter(c));
    it.trickplay = parseTrickplayMap(getObj(j, "Trickplay"));
    const Json& providers = getObj(j, "ProviderIds");
    for (auto p = providers.begin(); p != providers.end(); ++p)
        if (p->is_string()) it.providerIds[p.key()] = p->get<std::string>();

    it.canDownload = getBool(j, "CanDownload");
    it.path = getStr(j, "Path");
    it.container = getStr(j, "Container");
    it.size = getI64(j, "Size");
    if (it.size <= 0 && !it.mediaSources.empty()) it.size = it.mediaSources.front().size;
    if (it.container.empty() && !it.mediaSources.empty()) it.container = it.mediaSources.front().container;
    if (it.mediaStreams.empty() && !it.mediaSources.empty()) it.mediaStreams = it.mediaSources.front().mediaStreams;
    it.locationType = getStr(j, "LocationType");
    return it;
}

MediaSegment parseMediaSegment(const Json& j) {
    MediaSegment s;
    s.id = getStr(j, "Id");
    s.itemId = getStr(j, "ItemId");
    const std::string type = getStr(j, "Type");
    if (type == "Intro") s.type = MediaSegment::Type::Intro;
    else if (type == "Outro") s.type = MediaSegment::Type::Outro;
    else if (type == "Recap") s.type = MediaSegment::Type::Recap;
    else if (type == "Preview") s.type = MediaSegment::Type::Preview;
    else if (type == "Commercial") s.type = MediaSegment::Type::Commercial;
    s.startTicks = getI64(j, "StartTicks");
    s.endTicks = getI64(j, "EndTicks");
    return s;
}

RemoteSubtitleInfo parseRemoteSubtitle(const Json& j) {
    RemoteSubtitleInfo r;
    r.id = getStr(j, "Id");
    r.providerName = getStr(j, "ProviderName");
    r.name = getStr(j, "Name");
    r.format = getStr(j, "Format");
    r.language = getStr(j, "ThreeLetterISOLanguageName");
    r.author = getStr(j, "Author");
    r.comment = getStr(j, "Comment");
    r.dateCreated = getStr(j, "DateCreated");
    r.communityRating = getDouble(j, "CommunityRating");
    r.frameRate = getDouble(j, "FrameRate");
    r.downloadCount = getInt(j, "DownloadCount");
    r.isHashMatch = getBool(j, "IsHashMatch");
    r.forced = getBool(j, "Forced");
    r.hearingImpaired = getBool(j, "HearingImpaired");
    r.aiTranslated = getBool(j, "AiTranslated");
    r.machineTranslated = getBool(j, "MachineTranslated");
    return r;
}

SearchHint parseSearchHint(const Json& j) {
    SearchHint h;
    h.id = getStr(j, "Id", getStr(j, "ItemId"));
    h.name = getStr(j, "Name");
    h.type = getStr(j, "Type");
    h.mediaType = getStr(j, "MediaType");
    h.productionYear = getInt(j, "ProductionYear");
    h.indexNumber = getInt(j, "IndexNumber", -1);
    h.parentIndexNumber = getInt(j, "ParentIndexNumber", -1);
    h.series = getStr(j, "Series");
    h.primaryImageTag = getStr(j, "PrimaryImageTag");
    h.thumbImageTag = getStr(j, "ThumbImageTag");
    h.thumbImageItemId = getStr(j, "ThumbImageItemId");
    h.backdropImageTag = getStr(j, "BackdropImageTag");
    h.backdropImageItemId = getStr(j, "BackdropImageItemId");
    h.runTimeTicks = getI64(j, "RunTimeTicks");
    h.primaryImageAspectRatio = getDouble(j, "PrimaryImageAspectRatio");
    return h;
}

DiscoveredServer parseDiscoveryReply(const Json& j) {
    DiscoveredServer d;
    d.id = getStr(j, "Id");
    d.name = getStr(j, "Name");
    d.address = getStr(j, "Address");
    d.endpointAddress = getStr(j, "EndpointAddress");
    return d;
}

QueryResult<BaseItem> parseItems(const Json& j) { return parseQueryResult<BaseItem>(j, parseItem); }

int compareVersions(std::string_view a, std::string_view b) {
    auto next = [](std::string_view& s) -> int {
        int v = 0;
        size_t i = 0;
        while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
        while (i < s.size() && s[i] != '.') ++i;
        if (i < s.size()) ++i;
        s.remove_prefix(i);
        return v;
    };
    for (int part = 0; part < 4; ++part) {
        const int x = next(a);
        const int y = next(b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

}  // namespace karafin::jellyfin
