#include <format>
#include <string>

#include "core/log.hpp"
#include "jellyfin/jellyfin.hpp"

namespace karafin::jellyfin {

namespace {

using Json = nlohmann::json;

constexpr const char* kEpisodeFixture = R"json({
  "Id": "ep1", "Name": "Pilot", "Type": "Episode", "MediaType": "Video", "ServerId": "srv",
  "SeriesId": "series1", "SeriesName": "Show", "SeasonId": "season1", "SeasonName": "Season 1",
  "IndexNumber": "3", "ParentIndexNumber": 1, "CommunityRating": "7.5", "ProductionYear": null,
  "RunTimeTicks": 26000000000, "Overview": "A start.", "CanDownload": true, "Genres": ["Drama", 5],
  "UserData": {"PlaybackPositionTicks": 13000000000, "Played": false, "IsFavorite": true},
  "ImageTags": {"Thumb": "thumbtag"}, "BackdropImageTags": [],
  "ParentBackdropItemId": "series1", "ParentBackdropImageTags": ["bdtag"],
  "SeriesPrimaryImageTag": "sptag", "ParentLogoItemId": "series1", "ParentLogoImageTag": "logotag",
  "People": [{"Name": "Actor A", "Id": "p1", "Role": "Hero", "Type": "Actor", "PrimaryImageTag": "ptag"}],
  "Studios": [{"Name": "Studio", "Id": "s1"}],
  "Chapters": [{"StartPositionTicks": 0, "Name": "Intro"}, {"StartPositionTicks": 900000000, "Name": "Part 1"}],
  "Trickplay": {"ms1": {"320": {"Width": 320, "Height": 180, "TileWidth": 10, "TileHeight": 10,
                                "ThumbnailCount": 260, "Interval": 10000, "Bandwidth": 1000}}},
  "MediaSources": [{"Id": "ms1", "Container": "mkv", "Size": 1500000000, "ETag": "etag1",
                    "MediaStreams": [
                      {"Type": "Video", "Index": 0, "Codec": "hevc", "Width": 1920, "Height": 1080, "BitDepth": 10},
                      {"Type": "Audio", "Index": 1, "Codec": "eac3", "Language": "eng", "Channels": 6, "IsDefault": true},
                      {"Type": "Subtitle", "Index": 2, "Codec": "subrip", "Language": "eng", "IsExternal": true,
                       "Path": "/media/show/ep1.en.srt", "DisplayTitle": "English - SRT"}
                    ]}]
})json";

constexpr const char* kPlaybackInfoDirect = R"json({
  "PlaySessionId": "psid1",
  "MediaSources": [{
    "Id": "ms1", "Container": "mkv", "Protocol": "File", "ETag": "etag1",
    "SupportsDirectPlay": true, "SupportsDirectStream": true, "SupportsTranscoding": true,
    "DefaultAudioStreamIndex": 2, "DefaultSubtitleStreamIndex": -1,
    "MediaStreams": [
      {"Type": "Video", "Index": 0, "Codec": "h264"},
      {"Type": "Audio", "Index": 1, "Codec": "aac", "Language": "jpn"},
      {"Type": "Audio", "Index": 2, "Codec": "ac3", "Language": "eng", "IsDefault": true},
      {"Type": "Subtitle", "Index": 3, "Codec": "ass", "Language": "eng", "DeliveryMethod": "Embed"},
      {"Type": "Subtitle", "Index": 4, "Codec": "pgssub", "Language": "eng", "DeliveryMethod": "Embed"},
      {"Type": "Subtitle", "Index": 5, "Codec": "subrip", "Language": "fre", "IsExternal": true,
       "DeliveryMethod": "External", "DeliveryUrl": "/Videos/item1/ms1/Subtitles/5/0/Stream.srt"},
      {"Type": "Subtitle", "Index": 6, "Codec": "ass", "Language": "ger", "IsExternal": true}
    ]
  }]
})json";

constexpr const char* kPlaybackInfoTranscode = R"json({
  "PlaySessionId": "psid2",
  "MediaSources": [{
    "Id": "ms9", "Container": "avi", "SupportsDirectPlay": false, "SupportsDirectStream": false,
    "SupportsTranscoding": true, "TranscodingSubProtocol": "hls",
    "TranscodingUrl": "/videos/item2/master.m3u8?DeviceId=x&MediaSourceId=ms9&ApiKey=tok123",
    "MediaStreams": [{"Type": "Video", "Index": 0, "Codec": "mpeg4"}]
  }]
})json";

struct Checker {
    int passed = 0;
    int failed = 0;
    void expect(bool ok, std::string_view what) {
        if (ok) {
            ++passed;
        } else {
            ++failed;
            log::error("jellyfin-test", "FAILED: {}", what);
        }
    }
};

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

void testProfile(Checker& c) {
    DeviceProfileOptions o;
    o.maxStreamingBitrate = 40'000'000;
    const Json p = buildDeviceProfile(o);
    c.expect(p.value("MaxStreamingBitrate", 0) == 40'000'000, "profile bitrate");
    const Json& dp = p["DirectPlayProfiles"][0];
    c.expect(contains(dp.value("Container", ""), "mkv") && contains(dp.value("Container", ""), "m2ts"),
             "profile containers");
    c.expect(contains(dp.value("VideoCodec", ""), "hevc") && contains(dp.value("VideoCodec", ""), "av1"),
             "profile video codecs");
    c.expect(contains(dp.value("AudioCodec", ""), "truehd") && contains(dp.value("AudioCodec", ""), "opus"),
             "profile audio codecs");
    c.expect(p["TranscodingProfiles"][0].value("Protocol", "") == "hls", "profile hls transcoding");
    bool av1Capped = false;
    for (const auto& cp : p["CodecProfiles"])
        if (cp.value("Codec", "") == "av1")
            for (const auto& cond : cp["Conditions"])
                if (cond.value("Property", "") == "Height" && cond.value("Value", "") == "1080") av1Capped = true;
    c.expect(av1Capped, "profile av1 capped to 1080p");
    bool extSrt = false;
    bool embedPgs = false;
    for (const auto& sp : p["SubtitleProfiles"]) {
        if (sp.value("Format", "") == "srt" && sp.value("Method", "") == "External") extSrt = true;
        if (sp.value("Format", "") == "pgssub" && sp.value("Method", "") == "Embed") embedPgs = true;
    }
    c.expect(extSrt && embedPgs, "profile subtitle methods");
}

void testUrls(Checker& c) {
    auto a = serverUrlCandidates("192.168.1.5:8096");
    c.expect(a.size() == 2 && a[0] == "http://192.168.1.5:8096", "candidates ip:port");
    auto b = serverUrlCandidates(" jellyfin.example.com/ ");
    c.expect(b.size() == 3 && b[0] == "https://jellyfin.example.com" && b[1] == "http://jellyfin.example.com:8096",
             "candidates bare host");
    auto d = serverUrlCandidates("HTTPS://Jf.Example.com:443/jellyfin/web/index.html#!/home");
    c.expect(d.size() == 1 && d[0] == "https://jf.example.com/jellyfin", "candidates pasted web url");
    c.expect(serverUrlCandidates("http://").empty() && serverUrlCandidates("ftp://x").empty(), "candidates invalid");
    c.expect(stripApiPath("https://h.example/jf/System/Info/Public", "/System/Info/Public") == "https://h.example/jf",
             "stripApiPath");
    c.expect(compareVersions("10.9.11", "10.10.0") < 0 && compareVersions("10.10.3", "10.9") > 0 &&
                 compareVersions("10.8.0", "10.8") == 0,
             "compareVersions");
}

void testItem(Checker& c, const Client& client) {
    const BaseItem it = parseItem(Json::parse(kEpisodeFixture));
    c.expect(it.id == "ep1" && it.isEpisode() && it.indexNumber == 3 && it.parentIndexNumber == 1, "item basics");
    c.expect(it.episodeTag() == "S1:E3", "item episode tag");
    c.expect(it.communityRating > 7.4 && it.productionYear == 0, "item lenient numbers");
    c.expect(it.genres.size() == 1 && it.people.size() == 1 && it.studios.size() == 1, "item lists");
    c.expect(it.userData.isFavorite && it.progress() > 0.49 && it.progress() < 0.51, "item user data");
    c.expect(it.size == 1500000000 && it.container == "mkv" && it.mediaStreams.size() == 3, "item media fallback");
    c.expect(it.chapters.size() == 2 && it.canDownload, "item chapters/canDownload");

    const auto tpIt = it.trickplay.find("ms1");
    c.expect(tpIt != it.trickplay.end() && tpIt->second.count(320), "item trickplay");
    if (tpIt != it.trickplay.end() && tpIt->second.count(320)) {
        const TrickplayInfo& tp = tpIt->second.at(320);
        const auto tile = tp.tileAt(1'234'000);  // thumb 123 -> sheet 1, slot 23 -> col 3,row 2
        c.expect(tp.sheetCount() == 3 && tile.sheetIndex == 1 && tile.x == 960 && tile.y == 360, "trickplay tile");
    }

    ImageOptions opts;
    opts.maxWidth = 400;
    const std::string primary = client.imageUrl(it, ImageType::Primary, opts);
    c.expect(contains(primary, "/Items/series1/Images/Primary?") && contains(primary, "tag=sptag") &&
                 contains(primary, "format=Jpg") && contains(primary, "maxWidth=400"),
             "image primary falls back to series");
    c.expect(contains(client.imageUrl(it, ImageType::Backdrop), "/Items/series1/Images/Backdrop?"),
             "image backdrop falls back to parent");
    c.expect(contains(client.imageUrl(it, ImageType::Thumb), "/Items/ep1/Images/Thumb?"), "image own thumb");
    c.expect(contains(client.imageUrl(it, ImageType::Logo), "tag=logotag") &&
                 contains(client.imageUrl(it, ImageType::Logo), "format=Png"),
             "image logo parent keeps alpha");
    opts.fallback = false;
    c.expect(client.imageUrl(it, ImageType::Primary, opts).empty(), "image no fallback");

    const auto subs = client.externalSubtitleDownloads(it);
    c.expect(subs.size() == 1 && subs[0].fileName == "2.eng.srt" &&
                 contains(subs[0].url, "/Videos/ep1/ms1/Subtitles/2/Stream.srt?ApiKey=tok123"),
             "external subtitle downloads");
    c.expect(contains(client.downloadUrl("ep1"), "/Items/ep1/Download?ApiKey=tok123"), "download url");
    c.expect(contains(client.trickplayTileUrl("ep1", "ms1", 320, 2), "/Videos/ep1/Trickplay/320/2.jpg?mediaSourceId=ms1&ApiKey=tok123"),
             "trickplay tile url");
}

void testPlayback(Checker& c, const Client& client) {
    PlaybackRequest req;
    req.itemId = "item1";
    auto plan = client.resolvePlaybackInfo(Json::parse(kPlaybackInfoDirect), req);
    c.expect(plan.ok(), "plan resolves");
    if (plan.ok()) {
        c.expect(plan->method == PlayMethod::DirectPlay && plan->playSessionId == "psid1", "plan direct play");
        c.expect(contains(plan->url, "http://jf.local:8096/Videos/item1/stream?static=true&mediaSourceId=ms1") &&
                     contains(plan->url, "ApiKey=tok123") && contains(plan->url, "Tag=etag1"),
                 "plan static url");
        c.expect(plan->audioStreamIndex == 2 && plan->subtitleStreamIndex == -1, "plan default streams");
        c.expect(plan->externalSubtitles.size() == 2, "plan external subtitle count");
        if (plan->externalSubtitles.size() == 2) {
            c.expect(plan->externalSubtitles[0].url ==
                         "http://jf.local:8096/Videos/item1/ms1/Subtitles/5/0/Stream.srt?ApiKey=tok123",
                     "plan delivery url");
            c.expect(contains(plan->externalSubtitles[1].url, "/Videos/item1/ms1/Subtitles/6/Stream.ass?ApiKey="),
                     "plan built subtitle url");
        }
        c.expect(plan->mpvTrackId(2) == 2 && plan->mpvTrackId(4) == 2 && plan->mpvTrackId(5) == -1,
                 "plan mpv track ids");
        const PlaybackReport rep = PlaybackReport::fromPlan(*plan, 5 * kTicksPerSecond);
        const PlaybackReport back = PlaybackReport::fromJson(rep.toJson());
        c.expect(back.itemId == "item1" && back.playSessionId == "psid1" && back.positionTicks == 50'000'000 &&
                     back.audioStreamIndex == 2 && back.playMethod == PlayMethod::DirectPlay,
                 "report round trip");
    }
    req.itemId = "item2";
    auto tc = client.resolvePlaybackInfo(Json::parse(kPlaybackInfoTranscode), req);
    c.expect(tc.ok() && tc->method == PlayMethod::Transcode &&
                 tc->url == "http://jf.local:8096/videos/item2/master.m3u8?DeviceId=x&MediaSourceId=ms9&ApiKey=tok123",
             "plan transcode url");
    if (tc.ok()) c.expect(tc->mpvTrackId(0) == -1, "plan transcode has no mpv ids");
    auto bad = client.resolvePlaybackInfo(Json::parse(R"({"ErrorCode":"NotAllowed"})"), req);
    c.expect(!bad.ok() && bad.error.kind == ErrorKind::Forbidden, "plan error code");
}

void testMisc(Checker& c, const Client& client) {
    const auto subs = parseQueryResult<RemoteSubtitleInfo>(
        Json::parse(R"([{"Id":"os-1","ProviderName":"Open Subtitles","Name":"Rel","Format":"srt",
                         "ThreeLetterISOLanguageName":"eng","DownloadCount":"42","IsHashMatch":true,
                         "HearingImpaired":true}])"),
        parseRemoteSubtitle);
    c.expect(subs.items.size() == 1 && subs.items[0].downloadCount == 42 && subs.items[0].isHashMatch &&
                 subs.items[0].hearingImpaired && subs.totalRecordCount == 1,
             "remote subtitle parse");
    const auto qc = parseQuickConnect(Json::parse(R"({"Secret":"s3","Code":"123456","Authenticated":false})"));
    c.expect(qc.secret == "s3" && qc.code == "123456" && !qc.authenticated, "quick connect parse");
    const auto auth = parseAuthResult(Json::parse(
        R"({"User":{"Id":"u1","Name":"me","Policy":{"EnableContentDownloading":false}},"AccessToken":"t","ServerId":"srv"})"));
    c.expect(auth.accessToken == "t" && auth.user.id == "u1" && !auth.user.canDownload(), "auth parse");
    const auto seg = parseMediaSegment(Json::parse(R"({"Type":"Intro","StartTicks":0,"EndTicks":900000000})"));
    c.expect(seg.type == MediaSegment::Type::Intro && seg.endSeconds() == 90.0, "segment parse");
    const auto disc = parseDiscoveryReply(Json::parse(R"({"Address":"http://10.0.0.2:8096","Id":"abc","Name":"Home"})"));
    c.expect(disc.address == "http://10.0.0.2:8096" && disc.id == "abc", "discovery parse");

    http::Response r;
    r.status = 401;
    c.expect(ApiError::fromResponse(r, ErrorContext::Login).kind == ErrorKind::Unauthorized, "error 401 login");
    c.expect(ApiError::fromResponse(r, ErrorContext::Subtitles).kind == ErrorKind::Forbidden, "error 401 subtitles");
    r.status = 403;
    c.expect(contains(ApiError::fromResponse(r, ErrorContext::Download).message, "Allow media downloading"),
             "error 403 download");
    r.status = 404;
    c.expect(ApiError::fromResponse(r, ErrorContext::Subtitles).kind == ErrorKind::NotFound, "error 404 subtitles");
    r.status = 200;
    c.expect(ApiError::fromResponse(r).ok(), "error 200 ok");
    r.error = http::Error::Cancelled;
    c.expect(ApiError::fromResponse(r).kind == ErrorKind::Cancelled, "error cancelled");

    ItemsQuery q;
    q.parentId("lib").includeItemTypes("Movie").recursive().sortBy("SortName").limit(50).genres("Action|Drama");
    const auto params = q.toParams();
    auto has = [&](const char* k, const char* v) {
        for (const auto& [pk, pv] : params)
            if (pk == k && pv == v) return true;
        return false;
    };
    c.expect(has("ParentId", "lib") && has("Recursive", "true") && has("SortOrder", "Ascending") &&
                 has("Limit", "50") && has("Genres", "Action|Drama") && has("Fields", fields::kCard),
             "items query params");

    const std::string authHeader = client.authorizationHeader();
    c.expect(authHeader.starts_with("MediaBrowser Client=\"Karafin\", Device=\"Nintendo Switch\", DeviceId=\"") &&
                 contains(authHeader, "Version=\"" KARAFIN_VERSION "\"") && contains(authHeader, "Token=\"tok123\""),
             "authorization header");
    c.expect(contains(client.subtitleUrl("i", "m", 7, "vtt"), "/Videos/i/m/Subtitles/7/Stream.vtt?ApiKey=tok123"),
             "subtitle url");
    c.expect(client.withApiKey("/x?api_key=old") == "http://jf.local:8096/x?api_key=old&ApiKey=tok123" &&
                 client.withApiKey("/x?a=1&apikey=k") == "http://jf.local:8096/x?a=1&apikey=k",
             "withApiKey legacy name");
    r = http::Response{};
    r.status = 400;
    c.expect(ApiError::fromResponse(r, ErrorContext::Download).kind == ErrorKind::Invalid &&
                 contains(ApiError::fromResponse(r, ErrorContext::Download).message, "downloading this item"),
             "error 400 download");
    const Json profile = buildDeviceProfile();
    c.expect(profile.value("MaxStreamingBitrate", int64_t{0}) <= kMaxProfileBitrate, "profile bitrate fits int32");
}

}  // namespace

bool selfTest(std::string* summary) {
    Checker c;
    try {
        ServerInfo info;
        info.id = "srv";
        info.version = "10.9.11";
        auto client = Client::create("http://jf.local:8096/", info);
        UserDto user;
        user.id = "u1";
        user.name = "tester";
        client->setSession(user, "tok123");

        testProfile(c);
        testUrls(c);
        testItem(c, *client);
        testPlayback(c, *client);
        testMisc(c, *client);
        c.expect(client->baseUrl() == "http://jf.local:8096" && client->modernApi(), "client base url");
    } catch (const std::exception& e) {
        c.expect(false, std::string("exception: ") + e.what());
    }
    const std::string line = std::format("jellyfin self-test: {} passed, {} failed", c.passed, c.failed);
    if (c.failed) {
        log::error("jellyfin-test", "{}", line);
    } else {
        log::info("jellyfin-test", "{}", line);
    }
    if (summary) *summary = line;
    return c.failed == 0;
}

}  // namespace karafin::jellyfin
