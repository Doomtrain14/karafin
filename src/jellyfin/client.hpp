#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/http.hpp"
#include "jellyfin/accounts.hpp"
#include "jellyfin/call.hpp"
#include "jellyfin/extras.hpp"
#include "jellyfin/models.hpp"
#include "jellyfin/playback.hpp"
#include "jellyfin/query.hpp"

namespace karafin::jellyfin {

inline constexpr const char* kClientName = "Karafin";
inline constexpr const char* kDeviceName = "Nintendo Switch";
inline constexpr const char* kMinServerVersion = "10.8.0";

// One Jellyfin server session (base URL + optional signed-in user). Always held by shared_ptr;
// every method is thread-safe. API methods return a Call<T>: start it with .then(cb, token) on
// the main thread (callback on the main thread) or .wait() on a dedicated worker thread.
class Client : public std::enable_shared_from_this<Client> {
    struct Passkey {};

public:
    Client(Passkey, std::string baseUrl, ServerInfo info);

    // --- Creation ----------------------------------------------------------------------------
    // Normalizes what the user typed, probes the candidates via /System/Info/Public (following
    // redirects), checks that it is a Jellyfin server >= 10.8, saves it in AccountStore and returns
    // an unauthenticated client.
    static Call<std::shared_ptr<Client>> connect(std::string userInput);
    // No network: restores a saved server (and user token if given). Call validateSession() next.
    static std::shared_ptr<Client> fromStored(const StoredServer& server, const StoredUser* user = nullptr);
    // No network, no persistence: a client for an already-normalized base URL.
    static std::shared_ptr<Client> create(std::string baseUrl, ServerInfo info = {});

    // --- State ---------------------------------------------------------------------------------
    std::string baseUrl() const;
    ServerInfo serverInfo() const;
    std::string serverId() const;
    std::string userId() const;
    std::string accessToken() const;
    UserDto user() const;
    std::string deviceId() const { return deviceId_; }
    bool isAuthenticated() const;
    // True after a request failed to reach the server; cleared by the next successful response.
    bool isOffline() const { return offline_.load(); }
    // `MediaBrowser Client="Karafin", Device="Nintendo Switch", DeviceId="...", Version="1.0.0"[, Token="..."]`
    std::string authorizationHeader() const;
    // Applies credentials (also done automatically by the authenticate* calls).
    void setSession(const UserDto& user, const std::string& token);
    // Main-thread notifications. Unauthorized: an authenticated request got 401 (token revoked).
    void setUnauthorizedHandler(std::function<void()> fn);
    void setConnectivityHandler(std::function<void(bool offline)> fn);

    // --- Server & auth -----------------------------------------------------------------------
    Call<ServerInfo> publicInfo();                       // GET /System/Info/Public (also a ping)
    Call<std::vector<UserDto>> publicUsers();            // GET /Users/Public
    // POST /Users/AuthenticateByName. On success the session is applied and saved.
    Call<AuthResult> authenticateByName(std::string username, std::string password);
    Call<bool> quickConnectEnabled();                    // GET /QuickConnect/Enabled
    Call<QuickConnectState> quickConnectInitiate();      // POST (10.9+) or GET (10.8) /QuickConnect/Initiate
    Call<QuickConnectState> quickConnectState(std::string secret);  // GET /QuickConnect/Connect
    // POST /Users/AuthenticateWithQuickConnect. On success the session is applied and saved.
    Call<AuthResult> authenticateWithQuickConnect(std::string secret);
    // Startup check: refreshes /System/Info/Public, then GET /Users/Me. 401 clears the stored
    // token (go to login); Network/Offline errors mean "offline mode". On success flushes the
    // offline report queue.
    Call<UserDto> validateSession();
    Call<UserDto> currentUser();                         // GET /Users/Me
    // POST /Sessions/Logout (best effort), then forgets the token locally and in AccountStore.
    Call<Unit> logout();

    // --- Browsing ------------------------------------------------------------------------------
    Call<QueryResult<BaseItem>> userViews();
    Call<QueryResult<BaseItem>> resumeItems(int limit = 24);  // Continue Watching (video)
    Call<QueryResult<BaseItem>> nextUp(int limit = 24, std::string seriesId = {});
    Call<std::vector<BaseItem>> latest(std::string parentId, int limit = 20);
    Call<QueryResult<BaseItem>> items(const ItemsQuery& query);
    Call<BaseItem> item(std::string itemId);             // detail fields (+ Trickplay on 10.9+)
    Call<QueryResult<BaseItem>> seasons(std::string seriesId);
    // seasonId "" -> all episodes of the series. Includes MediaSources/Streams, Chapters, Trickplay.
    Call<QueryResult<BaseItem>> episodes(std::string seriesId, std::string seasonId = {});
    Call<QueryResult<BaseItem>> similar(std::string itemId, int limit = 12);
    Call<QueryResult<SearchHint>> searchHints(std::string term, int limit = 30,
                                              std::string includeItemTypes = "Movie,Series,Episode");
    Call<UserData> setFavorite(std::string itemId, bool favorite);
    Call<UserData> setPlayed(std::string itemId, bool played);

    // --- Images (no network; image endpoints need no auth) -------------------------------------
    // "" when neither the item nor (with fallback) its parents/series have that image.
    std::string imageUrl(const BaseItem& item, ImageType type, const ImageOptions& opts = {}) const;
    std::string itemImageUrl(const std::string& itemId, ImageType type, const std::string& tag,
                             const ImageOptions& opts = {}) const;
    std::string userImageUrl(const std::string& userId, const std::string& tag, const ImageOptions& opts = {}) const;
    std::string personImageUrl(const PersonInfo& person, const ImageOptions& opts = {}) const;
    std::string searchHintImageUrl(const SearchHint& hint, const ImageOptions& opts = {}) const;

    // --- Playback ------------------------------------------------------------------------------
    // POST /Items/{id}/PlaybackInfo with the Switch DeviceProfile and resolves the result.
    Call<PlaybackPlan> playbackPlan(PlaybackRequest request);
    // Pure function behind playbackPlan (exposed for tests): PlaybackInfo JSON -> plan.
    Result<PlaybackPlan> resolvePlaybackInfo(const nlohmann::json& playbackInfo, const PlaybackRequest& request) const;
    Call<Unit> reportPlaybackStart(PlaybackReport report);     // POST /Sessions/Playing
    Call<Unit> reportPlaybackProgress(PlaybackReport report);  // POST /Sessions/Playing/Progress
    // POST /Sessions/Playing/Stopped. If the server is unreachable (and queueIfOffline) the report
    // is queued in ReportQueue and the Call still fails with Network/Offline.
    Call<Unit> reportPlaybackStopped(PlaybackReport report, bool queueIfOffline = true);
    std::string staticStreamUrl(const std::string& itemId, const MediaSource& source,
                                const std::string& playSessionId = {}) const;
    std::string subtitleUrl(const std::string& itemId, const std::string& mediaSourceId, int streamIndex,
                            const std::string& format) const;

    // --- Extras --------------------------------------------------------------------------------
    Call<TrickplayMap> trickplay(std::string itemId);   // via item Fields=Trickplay (10.9+)
    std::string trickplayTileUrl(const std::string& itemId, const std::string& mediaSourceId, int width,
                                 int sheetIndex) const;
    // GET /MediaSegments/{id} (10.10+). Older servers / missing plugin -> empty list, not an error.
    Call<std::vector<MediaSegment>> mediaSegments(std::string itemId);
    // GET /Items/{id}/RemoteSearch/Subtitles/{lang}?isPerfectMatch=false. lang: ISO 639-2 ("eng").
    // Errors: Forbidden (no subtitle-management permission), NotFound (no provider / no results).
    Call<std::vector<RemoteSubtitleInfo>> searchRemoteSubtitles(std::string itemId, std::string language);
    // POST /Items/{id}/RemoteSearch/Subtitles/{subtitleId}.
    Call<Unit> downloadRemoteSubtitle(std::string itemId, std::string subtitleId);
    // Re-reads the item's MediaSources (after a subtitle download).
    Call<MediaSource> refreshMediaSource(std::string itemId, std::string mediaSourceId);
    // Download + poll (up to ~8 s) until the new external subtitle stream shows up.
    Call<SubtitleAddResult> downloadRemoteSubtitleAndRefresh(std::string itemId, std::string mediaSourceId,
                                                             std::string subtitleId);

    // --- Downloads data layer ------------------------------------------------------------------
    std::string downloadUrl(const std::string& itemId) const;  // /Items/{id}/Download?ApiKey=
    // Ready-to-send GET for the original file (auth header, no timeout, no compression). The
    // download manager adds rangeFrom / onData / onProgress / cancel and calls perform().
    http::Request downloadRequest(const std::string& itemId) const;
    // HEAD (falls back to a 1-byte ranged GET): size, ETag, range support. 403 -> Forbidden with
    // the "Allow media downloading" message.
    Call<DownloadProbe> probeDownload(std::string itemId);
    // Fresh check of Policy.EnableContentDownloading via /Users/Me.
    Call<bool> canDownload();
    // Item JSON (+ series and season JSON for episodes) for the offline store.
    Call<OfflineMetadata> offlineMetadata(std::string itemId);
    // External subtitle files of a media source, ready to download next to the video.
    std::vector<SubtitleDownload> externalSubtitleDownloads(const BaseItem& item,
                                                            const std::string& mediaSourceId = {}) const;

    // --- Low level -----------------------------------------------------------------------------
    // Request with base URL, Authorization and Accept headers. `path` starts with '/'.
    http::Request request(http::Method method, std::string_view path, const QueryParams& query = {}) const;
    Call<nlohmann::json> getJson(std::string path, QueryParams query = {});
    // Appends ApiKey to a server-relative or absolute URL (for mpv / downloads).
    std::string withApiKey(std::string_view urlOrPath) const;
    // True for servers >= 10.9 (new /UserItems, /UserViews ... routes); unknown version -> true.
    bool modernApi() const;

private:
    template <class T, class Parse>
    Call<T> jsonCall(http::Request req, ErrorContext ctx, Parse parse);
    Call<Unit> emptyCall(http::Request req, ErrorContext ctx);
    template <class T, class Parse>
    Result<T> parseResponse(const http::Response& resp, ErrorContext ctx, const Parse& parse);
    // Maps the response to an error and updates offline / unauthorized state.
    ApiError check(const http::Response& resp, ErrorContext ctx, bool authenticated);
    void noteReachable(bool reachable);
    void applyAuth(const AuthResult& auth);
    void setServerInfo(const ServerInfo& info);
    std::string userPath(std::string_view modern, std::string_view legacySuffix) const;
    QueryParams userParam() const;
    std::string withTrickplay(const char* fields) const;
    Call<Unit> postReport(const char* path, PlaybackReport report, bool queueOnFailure);

    mutable std::mutex mutex_;
    std::string baseUrl_;
    ServerInfo info_;
    UserDto user_;
    std::string token_;
    std::string deviceId_;
    std::atomic<bool> offline_{false};
    std::function<void()> onUnauthorized_;
    std::function<void(bool)> onConnectivity_;
};

using ClientPtr = std::shared_ptr<Client>;

// The app-wide active session (set by the login flow; read by screens/player/downloads).
// Thread-safe.
ClientPtr currentClient();
void setCurrentClient(ClientPtr client);

// Drives Quick Connect on the main thread: initiate -> show code -> poll -> authenticate.
//
//   qc_ = QuickConnectFlow::start(client,
//       [this](const std::string& code) { showCode(code); },
//       [this](Result<AuthResult> r) { r ? goHome() : showError(r.error.message); });
//   ...  qc_->cancel();   // when the screen closes
class QuickConnectFlow : public std::enable_shared_from_this<QuickConnectFlow> {
public:
    using CodeCallback = std::function<void(const std::string& code)>;
    using DoneCallback = std::function<void(Result<AuthResult>)>;

    static std::shared_ptr<QuickConnectFlow> start(ClientPtr client, CodeCallback onCode, DoneCallback onDone,
                                                   double pollSeconds = 3.0, double timeoutSeconds = 300.0);
    void cancel();
    const std::string& code() const { return state_.code; }

private:
    void poll();
    void finish(Result<AuthResult> result);

    ClientPtr client_;
    CodeCallback onCode_;
    DoneCallback onDone_;
    QuickConnectState state_;
    CancelToken token_;
    double pollSeconds_ = 3.0;
    double remaining_ = 300.0;
    bool done_ = false;
};

}  // namespace karafin::jellyfin
