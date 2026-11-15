# `src/jellyfin/`: Jellyfin API client

Namespace `karafin::jellyfin`. Depends on `core` only. Umbrella include: `#include "jellyfin/jellyfin.hpp"`.
Targets Jellyfin **10.8+**. On 10.9+ it uses the newer routes (`/UserViews`, `/UserItems/Resume`,
`/Items?userId=`, `/UserFavoriteItems`, ...). On 10.8 it uses the legacy `/Users/{id}/...` routes. The
choice depends on `ServerInfo.version`; see `Client::modernApi()`.

## Files

| File | Contents |
| --- | --- |
| `error.hpp` | `ErrorKind`, `ErrorContext`, `ApiError` (HTTP status, user-facing message, technical detail) |
| `call.hpp` | `Result<T>`, `Status`, `Call<T>` (async `then` / blocking `wait`) |
| `models.hpp` | DTOs and lenient parsers: `ServerInfo`, `UserDto`/`UserPolicy`, `AuthResult`, `QuickConnectState`, `BaseItem`, `UserData`, `MediaSource`, `MediaStream`, `ChapterInfo`, `PersonInfo`, `TrickplayInfo`, `MediaSegment`, `RemoteSubtitleInfo`, `SearchHint`, `QueryResult<T>`, `DiscoveredServer` |
| `query.hpp` | `ItemsQuery` fluent builder for `/Items`, plus the `fields::kCard/kDetail/kEpisode` field sets |
| `client.hpp` | `Client` (one per server session), `currentClient()`/`setCurrentClient()`, `QuickConnectFlow` |
| `playback.hpp` | `PlaybackRequest`, `PlaybackPlan`, `ExternalSubtitle`, `PlaybackReport`, `PlayMethod` |
| `extras.hpp` | `ImageType`/`ImageOptions`, `DownloadProbe`, `OfflineMetadata`, `SubtitleDownload`, `SubtitleAddResult` |
| `device_profile.hpp` | `DeviceProfileOptions` (reads settings), `buildDeviceProfile()` |
| `accounts.hpp` | `AccountStore`: servers/users/tokens and the stable DeviceId |
| `report_queue.hpp` | `ReportQueue`: persistent offline playback reports |
| `discovery.hpp` | `discoverServers()` and `discoverServersBlocking()` (UDP 7359) |
| `url.hpp` | `serverUrlCandidates()`, `normalizeBaseUrl()` |
| `jellyfin.hpp` | umbrella include, plus `selfTest()` |

## Calls, results and errors

Every network method returns a `Call<T>`. Nothing is sent until you start it:

```cpp
// UI / main thread: the callback runs on the main thread. If token_ was cancelled first, the
// callback is NOT invoked, so capturing `this` is safe as long as the screen cancels token_ on pop.
client->items(q).then([this](Result<QueryResult<BaseItem>> r) {
    if (!r) return showError(r.error.message);
    fill(r->items);
}, token_);

// Dedicated worker thread (e.g. the download thread): blocks, never call on the main thread.
Result<BaseItem> r = client->item(id).wait(cancelToken);
```

* Async mode: the HTTP transfer runs on the core `http` thread. JSON parsing and any follow-up
  requests run on `workers()`, and only the final `Result<T>` is posted to the main thread.
* `Call<T>::map(f)` transforms the value. `mapError(f)` adjusts an error. `Call<T>::ready/fail`
  build an already-finished call.
* `Result<T>`: `ok()` / `operator bool`, `value` (`*r`, `r->`), `error`.
* `ApiError { ErrorKind kind; long status; std::string message /*for the user*/; std::string detail /*for logs*/; }`
  * Kinds: `Network`, `Offline` (the console has no network), `Unauthorized`, `Forbidden`,
    `NotFound`, `Invalid`, `Server`, `Parse`, `Cancelled`.
  * `isConnectivity()` is true for Network or Offline, which means "show offline mode / retry later".
  * Messages depend on the operation's `ErrorContext`. For example, 401 during Login gives
    "Wrong username or password", and 403 during a download names the "Allow media downloading"
    permission.

## Sessions and persistence

| What | Where |
| --- | --- |
| Servers, users, access tokens, DeviceId | `sdmc:/switch/karafin/jellyfin_accounts.json` (atomic writes, `AccountStore`) |
| Pending offline playback reports | `sdmc:/switch/karafin/jellyfin_pending_reports.json` (`ReportQueue`) |
| Playback tuning | `settings.json` keys `jellyfin.max_streaming_bitrate` (bits/s, default 120000000), `jellyfin.max_audio_channels` (default 8), `jellyfin.enable_av1` (default true) |

The DeviceId is `karafin-<32 hex>`. It is generated once with the kernel CSPRNG and then reused.
Every request sends:
`Authorization: MediaBrowser Client="Karafin", Device="Nintendo Switch", DeviceId="...", Version="1.0.0", Token="..."`.

### Startup / login flow (screens)

```cpp
if (auto last = AccountStore::instance().lastSession()) {
    auto client = Client::fromStored(last->server, &last->user);   // no network yet
    client->validateSession().then([client](Result<UserDto> r) {
        if (r) { setCurrentClient(client); goHome(); }
        else if (r.error.kind == ErrorKind::Unauthorized) goLogin(client);  // token was cleared
        else if (r.error.isConnectivity()) goOfflineDownloads();
        else showError(r.error.message);
    }, token_);
}

// Adding a server: accepts "192.168.1.5:8096", "jf.example.com", "https://h/jellyfin/web/#!/home"...
Client::connect(userText).then([](Result<ClientPtr> r) { /* r.value is unauthenticated; server saved */ }, token_);
discoverServers([](Result<std::vector<DiscoveredServer>> r) { /* r->at(i).address -> Client::connect */ });

client->publicUsers();                                   // avatar grid: userImageUrl(u.id, u.primaryImageTag)
client->authenticateByName(name, password);             // applies + saves the session on success
auto qc = QuickConnectFlow::start(client, onCode, onDone); // or quickConnectEnabled/Initiate/State/authenticateWithQuickConnect
client->logout();                                        // clears the token locally even when offline
```

* `AccountStore::servers()` returns the saved servers, most recent first. Each server's `users`
  list is also most-recent-first; a user with `hasToken()` can sign in without a password.
  `removeServer`/`removeUser`/`markUsed` manage the lists.
* `Client::setUnauthorizedHandler(fn)` runs on the main thread when an authenticated request gets
  a 401, meaning the token was revoked.
* `Client::setConnectivityHandler(fn(bool offline))` runs on the main thread whenever
  reachability changes. `isOffline()` reports the last known state. A failed request doesn't
  count as an outage while any transfer (downloads included) has received data in the last 15
  seconds. Otherwise the client goes offline after three failures in a row, or on any failure
  once nothing has succeeded for 15 seconds.

## Browsing

`userViews()`, `resumeItems(limit)`, `nextUp(limit, seriesId)`, `latest(parentId, limit)`,
`items(ItemsQuery)`, `item(id)` (detail fields, plus Trickplay on 10.9+), `seasons(seriesId)`,
`episodes(seriesId, seasonId = "")` (with MediaSources, MediaStreams, Overview, Chapters and
Trickplay), `similar(id)`, `searchHints(term)`, `setFavorite(id, bool)`, `setPlayed(id, bool)`.
The last two return the updated `UserData`.

```cpp
ItemsQuery q;
q.parentId(lib.id).includeItemTypes("Movie").recursive().sortBy("DateCreated", "Descending")
 .filters("IsUnplayed").startIndex(offset).limit(60);
```

### Images (no network, no auth needed)

```cpp
client->imageUrl(item, ImageType::Primary, {.maxWidth = 300});
client->imageUrl(item, ImageType::Backdrop, {.maxWidth = 1280, .quality = 80});
```

Images are requested as JPEG (`format=Jpg`) with `tag` for cache-busting. Fallbacks apply when the
item has no image of the requested type:

| Type | Fallback order |
| --- | --- |
| Primary | parent primary, then series primary |
| Backdrop | parent backdrop |
| Thumb | parent thumb, then series thumb, then backdrop |
| Logo | parent logo |

Turn fallbacks off with `ImageOptions::fallback = false`. The result is `""` when there is no
image at all. Related helpers: `userImageUrl`, `personImageUrl`, `searchHintImageUrl`.

## Playback (player)

```cpp
PlaybackRequest req;
req.itemId = item.id;
req.mediaSourceId = sourceId;                  // optional
req.startTimeTicks = item.userData.playbackPositionTicks;
req.audioStreamIndex = 2;                      // optional; subtitleStreamIndex = -1 turns subs off
client->playbackPlan(req).then([this](Result<PlaybackPlan> r) {
    if (!r) return showError(r.error.message);
    mpv.loadfile(r->url);                                  // static stream or HLS master playlist
    for (auto& s : r->externalSubtitles) mpv.subAdd(s.url, s.title, s.language);
    int sid = r->mpvTrackId(r->subtitleStreamIndex);       // embedded tracks -> mpv sid/aid (direct play only)
    report_ = PlaybackReport::fromPlan(*r, startTicks);
    client->reportPlaybackStart(report_).then({});
}, token_);
```

* `playbackPlan` POSTs `/Items/{id}/PlaybackInfo` with `buildDeviceProfile(DeviceProfileOptions::fromSettings())`,
  then picks a play method in this order:
  1. **DirectPlay**: `/Videos/{id}/stream?static=true&mediaSourceId=...&deviceId=...&Tag=...&PlaySessionId=...&ApiKey=...`
  2. **DirectStream**: the same static URL
  3. **Transcode**: the server's `TranscodingUrl` (HLS ts/h264/aac), with `ApiKey` appended
* Media URLs authenticate with the `ApiKey` query parameter (10.8+). Jellyfin 10.11 ignores the
  old `api_key` name unless the admin enables legacy authorization, so `withApiKey()` adds
  `ApiKey` even to server URLs that only carry `api_key`.
* `ErrorCode` values map to clear errors: NotAllowed → Forbidden, NoCompatibleStream → Server,
  RateLimitExceeded → Server.
* `externalSubtitles` lists:
  * every subtitle with `DeliveryMethod == External` (uses the server's `DeliveryUrl` plus `ApiKey`)
  * every external file without a delivery URL
    (`/Videos/{id}/{msId}/Subtitles/{index}/Stream.{fmt}?ApiKey=`)
* Embedded tracks stay inside the container. Select them with mpv `sid`/`aid` from `mpvTrackId()`.
* Reporting:
  * `reportPlaybackStart(report)` on start.
  * `reportPlaybackProgress(report)` every ~10 s, and on pause/unpause or track changes (set
    `report.eventName`, `isPaused`, `audioStreamIndex`, `subtitleStreamIndex`).
  * `reportPlaybackStopped(report)` at the end, with `positionTicks` updated each time.
  * If Stopped can't reach the server it is **queued automatically** in `ReportQueue`.
* `DeviceProfile` details:
  * Direct play containers: mkv/webm/mp4/m4v/mov/avi/ts/m2ts.
  * Video: h264, hevc, vp9, vp8, av1, mpeg2video, mpeg4, vc1.
  * Audio: aac, ac3, eac3, flac, mp3, opus, vorbis, dts, truehd, pcm, ...
  * Codec limits: H.264 8-bit ≤ L5.2; HEVC Main/Main10 ≤ L5.1, HDR10/HDR10+/HLG and Dolby
    Vision with an HDR10/HLG/SDR base layer (profile 5 without fallback is transcoded);
    AV1 ≤ 1920x1080 and ≤ 60 fps.
  * Bitrates are clamped to the server's int32 fields (`kMaxProfileBitrate`).
  * Subtitles: Embed for srt/ass/ssa/vtt/subrip/pgssub/dvdsub/...; External for srt/ass/ssa/vtt.

## Subtitles: remote search and download (subtitle manager)

```cpp
client->searchRemoteSubtitles(itemId, "eng").then([](Result<std::vector<RemoteSubtitleInfo>> r) {
    // r->items: providerName, name, format, communityRating, downloadCount, hearingImpaired, isHashMatch
    // Sorted with hash matches first, then by downloads.
});
client->downloadRemoteSubtitleAndRefresh(itemId, plan.mediaSourceId, chosen.id)
    .then([](Result<SubtitleAddResult> r) {
        if (r && r->newStream) { mpv.subAdd(r->url); mpv.setSid(<new track>); }   // no playback restart
    });
```

* The search language is ISO 639-2 (`eng`, `fre`, `ger`, ...). The request uses `isPerfectMatch=false`.
* Errors:
  * 401/403 → `Forbidden`: "no permission to download subtitles". The user needs subtitle
    management rights on the server.
  * 404, or an empty result list → `NotFound`: "no results / no subtitle provider (OpenSubtitles plugin)".
* The server indexes a downloaded subtitle in a queued refresh. `downloadRemoteSubtitleAndRefresh`
  POSTs the download, then polls `refreshMediaSource` once a second for up to ~8 s until a new
  subtitle stream appears.
  * If it doesn't appear in time, `newStream` is empty but the call still succeeds. Retry
    `refreshMediaSource()` later.
* Building blocks: `downloadRemoteSubtitle()`, `refreshMediaSource()`, `subtitleUrl()`.

## Trickplay, media segments

* Trickplay info comes from `item.trickplay[mediaSourceId][width]`, or from `client->trickplay(itemId)`.
  * `TrickplayInfo::tileAt(positionMs)` returns the sheet index plus the pixel rect of the thumbnail.
  * The sheet image URL is `trickplayTileUrl(itemId, msId, width, sheetIndex)`
    (`/Videos/{id}/Trickplay/{width}/{index}.jpg`, authenticated via `ApiKey`).
* `mediaSegments(itemId)` returns Intro/Outro (credits)/Recap/Preview/Commercial segments sorted by
  start. Servers older than 10.10 return an empty list.

## Downloads data layer (download manager)

| Call | Use |
| --- | --- |
| `client->canDownload()` / `user().canDownload()` | Policy `EnableContentDownloading` (fresh / cached). `BaseItem::canDownload` is per item. |
| `client->probeDownload(id)` | `DownloadProbe{url, contentLength, eTag, lastModified, contentType, fileName, acceptRanges}`. Tries HEAD, then falls back to a 1-byte ranged GET. 403 → Forbidden with the permission message. |
| `client->downloadRequest(id)` | Ready `http::Request` for `/Items/{id}/Download`: auth header, `timeoutMs = 0`, no compression, 60 s low-speed abort. Add `rangeFrom`, `onData`, `onProgress`, `cancel`, then call `http::Client::perform()` on the download thread. |
| `client->downloadUrl(id)` | `/Items/{id}/Download?ApiKey=...` for logging or external tools |
| `client->offlineMetadata(id)` | `OfflineMetadata{item, series, season (raw JSON), parsed}`. Store the JSON next to the file; re-parse with `parseItem()` when offline. |
| `client->externalSubtitleDownloads(item, msId)` | External subtitle files to bundle: `{index, url, format, language, fileName = "<index>.<lang>[.forced][.sdh].<fmt>"}` |
| `client->episodes(seriesId, seasonId)` | Expands a season or series into episodes |
| `client->imageUrl(...)` | Poster and backdrop to bundle |
| `ReportQueue::instance().enqueueStopped(serverId, userId, report)` | Progress from offline playback. Use `enqueuePlayed(...)` for watched or unwatched. |

* Pending reports are flushed automatically. This happens when a `Client` for the same server and
  user validates its session or regains connectivity. The flush runs on worker threads (the
  queue file is rewritten per delivered report); only its `done` callback runs on the main thread.
* You can also flush manually with `ReportQueue::instance().flush(client, done)`.
* Only the latest Stopped report per item is kept.

All of these methods also work with `.wait()` on the download thread.

## Self-test

`jellyfin::selfTest(&summary)` runs entirely offline. It builds the DeviceProfile, then checks the
parsers, URL normalization, image fallbacks, plan resolution (direct play, transcode, error codes),
mpv track mapping, trickplay tile math, error mapping, the query builder and the auth header,
using JSON fixtures embedded in `selftest.cpp`. Failures are logged under the `jellyfin-test`
tag. It returns true when everything passes. Call it from a debug path, for example at startup
in Debug builds.

## Limitations

* Targets the Jellyfin 10.8-10.11 API.
* Discovery uses the IPv4 broadcast 255.255.255.255 only, and needs `platform::networkConnected()`.
* Queued offline Stopped reports overwrite the server's resume position even if the item was
  watched elsewhere after the offline session.
* Remote subtitle download depends on a server-side provider plugin. A subtitle that the server
  doesn't index within ~8 s is reported as `newStream == nullopt`.
* Music, Live TV and transcoded downloads are out of scope.
