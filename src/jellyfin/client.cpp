#include "jellyfin/client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "jellyfin/detail/client_impl.hpp"
#include "jellyfin/detail/json_get.hpp"
#include "jellyfin/report_queue.hpp"
#include "jellyfin/url.hpp"

namespace karafin::jellyfin {

using Json = nlohmann::json;

namespace {

std::string buildAuthHeader(const std::string& deviceId, const std::string& token) {
    // Values are quoted; Jellyfin URL-decodes them, so escape the few characters that matter.
    auto esc = [](const std::string& v) {
        std::string out;
        for (char c : v) {
            if (c == '"' || c == ',' || c == '%') {
                out += http::urlEncode(std::string_view(&c, 1));
            } else {
                out += c;
            }
        }
        return out;
    };
    std::string h = std::string("MediaBrowser Client=\"") + kClientName + "\", Device=\"" + kDeviceName +
                    "\", DeviceId=\"" + esc(deviceId) + "\", Version=\"" KARAFIN_VERSION "\"";
    if (!token.empty()) h += ", Token=\"" + esc(token) + "\"";
    return h;
}

std::mutex g_currentMutex;
ClientPtr g_current;

// Prefers the most specific failure across URL candidates: anything over a plain network error.
void keepBestError(ApiError& best, const ApiError& e) {
    if (best.ok() || (best.kind == ErrorKind::Network && e.kind != ErrorKind::Network)) best = e;
}

void tryCandidate(const detail::Exec& exec, std::shared_ptr<std::vector<std::string>> candidates, size_t i,
                  ApiError best, Callback<ClientPtr> done) {
    if (i >= candidates->size()) {
        if (best.ok()) best = ApiError::make(ErrorKind::Network, "Could not reach the server.");
        done(Result<ClientPtr>::failure(best));
        return;
    }
    const std::string base = (*candidates)[i];
    http::Request req;
    req.url = http::joinUrl(base, "/System/Info/Public");
    req.headers = {{"Accept", "application/json"},
                   {"Authorization", buildAuthHeader(AccountStore::instance().deviceId(), {})}};
    req.timeoutMs = 8000;
    req.connectTimeoutMs = 5000;
    exec.send(std::move(req), [exec, candidates, i, best, done, base](http::Response resp) mutable {
        if (resp.cancelled()) return done(Result<ClientPtr>::failure(ApiError::cancelled()));
        ApiError err = ApiError::fromResponse(resp, ErrorContext::Connect);
        ServerInfo info;
        if (err.ok()) {
            const Json j = resp.json();
            info = parseServerInfo(j);
            const bool looksJellyfin =
                j.is_object() && !info.id.empty() &&
                (info.productName.empty() || info.productName.find("Jellyfin") != std::string::npos);
            if (!looksJellyfin) {
                err = ApiError::make(ErrorKind::Invalid, "That address doesn't point to a Jellyfin server.",
                                     resp.status, "no Jellyfin server info at " + base);
            } else if (!info.version.empty() && compareVersions(info.version, kMinServerVersion) < 0) {
                err = ApiError::make(ErrorKind::Invalid,
                                     "Jellyfin " + info.version + " is too old. Karafin needs " +
                                         std::string(kMinServerVersion) + " or newer.",
                                     resp.status);
            } else if (!info.startupWizardCompleted) {
                err = ApiError::make(ErrorKind::Invalid, "Finish the Jellyfin setup wizard on the server first.",
                                     resp.status);
            }
        }
        if (!err.ok()) {
            log::info("jellyfin", "connect candidate {} failed: {}", base, err.describe());
            keepBestError(best, err);
            tryCandidate(exec, candidates, i + 1, best, done);
            return;
        }
        std::string finalBase = resp.effectiveUrl.empty() ? base : stripApiPath(resp.effectiveUrl, "/System/Info/Public");
        if (finalBase.empty()) finalBase = base;
        log::info("jellyfin", "connected to \"{}\" {} at {}", info.name, info.version, finalBase);
        AccountStore::instance().saveServer(info, finalBase);
        done(Result<ClientPtr>::success(Client::create(finalBase, info)));
    });
}

}  // namespace

// --- Construction / state ----------------------------------------------------------------------

Client::Client(Passkey, std::string baseUrl, ServerInfo info)
    : baseUrl_(std::move(baseUrl)), info_(std::move(info)), deviceId_(AccountStore::instance().deviceId()) {}

ClientPtr Client::create(std::string baseUrl, ServerInfo info) {
    std::string normalized = normalizeBaseUrl(baseUrl);
    return std::make_shared<Client>(Passkey{}, normalized.empty() ? std::move(baseUrl) : std::move(normalized),
                                    std::move(info));
}

ClientPtr Client::fromStored(const StoredServer& server, const StoredUser* user) {
    ServerInfo info;
    info.id = server.id;
    info.name = server.name;
    info.version = server.version;
    auto client = create(server.url, info);
    if (user && user->hasToken()) {
        UserDto u;
        u.id = user->id;
        u.name = user->name;
        u.serverId = server.id;
        u.primaryImageTag = user->primaryImageTag;
        std::lock_guard lock(client->mutex_);
        client->user_ = std::move(u);
        client->token_ = user->token;
    }
    return client;
}

Call<ClientPtr> Client::connect(std::string userInput) {
    auto candidates = std::make_shared<std::vector<std::string>>(serverUrlCandidates(userInput));
    if (candidates->empty())
        return Call<ClientPtr>::fail(
            ApiError::make(ErrorKind::Invalid, "That doesn't look like a server address (e.g. 192.168.1.10:8096)."));
    return Call<ClientPtr>([candidates](const detail::Exec& exec, Callback<ClientPtr> done) {
        tryCandidate(exec, candidates, 0, ApiError{}, std::move(done));
    });
}

std::string Client::baseUrl() const {
    std::lock_guard lock(mutex_);
    return baseUrl_;
}

ServerInfo Client::serverInfo() const {
    std::lock_guard lock(mutex_);
    return info_;
}

std::string Client::serverId() const {
    std::lock_guard lock(mutex_);
    return info_.id;
}

std::string Client::userId() const {
    std::lock_guard lock(mutex_);
    return user_.id;
}

std::string Client::accessToken() const {
    std::lock_guard lock(mutex_);
    return token_;
}

UserDto Client::user() const {
    std::lock_guard lock(mutex_);
    return user_;
}

bool Client::isAuthenticated() const {
    std::lock_guard lock(mutex_);
    return !token_.empty() && !user_.id.empty();
}

std::string Client::authorizationHeader() const { return buildAuthHeader(deviceId_, accessToken()); }

void Client::setSession(const UserDto& user, const std::string& token) {
    std::lock_guard lock(mutex_);
    user_ = user;
    token_ = token;
}

void Client::setUnauthorizedHandler(std::function<void()> fn) {
    std::lock_guard lock(mutex_);
    onUnauthorized_ = std::move(fn);
}

void Client::setConnectivityHandler(std::function<void(bool)> fn) {
    std::lock_guard lock(mutex_);
    onConnectivity_ = std::move(fn);
}

bool Client::modernApi() const {
    std::lock_guard lock(mutex_);
    return info_.version.empty() || compareVersions(info_.version, "10.9.0") >= 0;
}

void Client::setServerInfo(const ServerInfo& info) {
    std::lock_guard lock(mutex_);
    const std::string keepId = info_.id;
    info_ = info;
    if (info_.id.empty()) info_.id = keepId;
}

void Client::applyAuth(const AuthResult& auth) {
    ServerInfo info;
    std::string base;
    {
        std::lock_guard lock(mutex_);
        user_ = auth.user;
        token_ = auth.accessToken;
        if (info_.id.empty()) info_.id = auth.serverId;
        info = info_;
        base = baseUrl_;
    }
    log::info("jellyfin", "signed in as {} on {}", auth.user.name, base);
    AccountStore::instance().saveServer(info, base);
    AccountStore::instance().saveUser(info.id, auth.user, auth.accessToken);
}

void Client::noteReachable(bool reachable) {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (reachable) {
        failStreak_.store(0);
        lastReachableMs_.store(now);
    } else if (!offline_.load()) {
        const int streak = failStreak_.fetch_add(1) + 1;
        // Bytes arriving on any transfer (downloads come from this same server) mean the link is
        // up and this request was just slow, e.g. queued behind saturating downloads.
        const int64_t quiet = http::msSinceLastReceive();
        if (quiet >= 0 && quiet < kOfflineGraceMs) {
            log::info("jellyfin", "request failed while data is still flowing; staying online");
            return;
        }
        // With nothing flowing, go offline after repeated failures or once nothing has succeeded
        // for a while.
        const int64_t last = lastReachableMs_.load();
        if (streak < kOfflineFailureStreak && last != 0 && now - last < kOfflineGraceMs) return;
    }
    const bool wasOffline = offline_.exchange(!reachable);
    if (wasOffline == !reachable) return;
    log::info("jellyfin", "server {} is {}", baseUrl(), reachable ? "reachable again" : "unreachable");
    std::function<void(bool)> handler;
    {
        std::lock_guard lock(mutex_);
        handler = onConnectivity_;
    }
    if (handler) main_thread::post([handler, reachable] { handler(!reachable); });
    if (reachable && isAuthenticated()) ReportQueue::instance().flush(shared_from_this());
}

ApiError Client::check(const http::Response& resp, ErrorContext ctx, bool authenticated) {
    ApiError e = ApiError::fromResponse(resp, ctx);
    if (e.kind == ErrorKind::Cancelled) return e;
    noteReachable(!e.isConnectivity());
    if (e) log::warn("jellyfin", "{} -> {}", resp.effectiveUrl, e.describe());
    if (resp.status == 401 && authenticated && ctx != ErrorContext::Login && ctx != ErrorContext::QuickConnect) {
        std::function<void()> handler;
        {
            std::lock_guard lock(mutex_);
            handler = onUnauthorized_;
        }
        if (handler) main_thread::post(handler);
    }
    return e;
}

http::Request Client::request(http::Method method, std::string_view path, const QueryParams& query) const {
    http::Request r;
    r.method = method;
    r.url = http::joinUrl(baseUrl(), path);
    if (!query.empty()) {
        r.url += r.url.find('?') == std::string::npos ? '?' : '&';
        r.url += http::buildQuery(query);
    }
    r.headers = {{"Authorization", authorizationHeader()}, {"Accept", "application/json"}};
    r.timeoutMs = 20000;
    return r;
}

Call<Unit> Client::emptyCall(http::Request req, ErrorContext ctx) {
    auto self = shared_from_this();
    return Call<Unit>([self, req = std::move(req), ctx](const detail::Exec& exec, Callback<Unit> done) {
        exec.send(req, [self, ctx, done](http::Response resp) {
            const bool authed = !self->accessToken().empty();
            if (ApiError e = self->check(resp, ctx, authed)) return done(Status::failure(std::move(e)));
            done(Status::success({}));
        });
    });
}

Call<Json> Client::getJson(std::string path, QueryParams query) {
    return jsonCall<Json>(request(http::Method::Get, path, query), ErrorContext::Generic,
                          [](const Json& j) { return j; });
}

std::string Client::withApiKey(std::string_view urlOrPath) const {
    std::string url(urlOrPath);
    if (url.find("://") == std::string::npos) url = http::joinUrl(baseUrl(), url);
    const std::string token = accessToken();
    if (token.empty()) return url;
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // `ApiKey` works on 10.8+; `api_key` is ignored by 10.11+ unless EnableLegacyAuthorization is on,
    // so a URL carrying only the legacy name still gets ApiKey.
    if (lower.find("?apikey=") != std::string::npos || lower.find("&apikey=") != std::string::npos) return url;
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += "ApiKey=" + http::urlEncode(token);
    return url;
}

std::string Client::userPath(std::string_view modern, std::string_view legacySuffix) const {
    if (modernApi()) return std::string(modern);
    return "/Users/" + userId() + std::string(legacySuffix);
}

QueryParams Client::userParam() const { return {{"userId", userId()}}; }

std::string Client::withTrickplay(const char* fieldList) const {
    std::string f(fieldList);
    if (modernApi()) f += ",Trickplay";
    return f;
}

// --- Server & auth -----------------------------------------------------------------------------

Call<ServerInfo> Client::publicInfo() {
    auto self = shared_from_this();
    auto req = request(http::Method::Get, "/System/Info/Public");
    req.timeoutMs = 10000;
    return jsonCall<ServerInfo>(std::move(req), ErrorContext::Connect, [self](const Json& j) -> Result<ServerInfo> {
        ServerInfo info = parseServerInfo(j);
        if (info.id.empty()) return Result<ServerInfo>::failure(ApiError::parse("System/Info/Public without Id"));
        self->setServerInfo(info);
        return Result<ServerInfo>::success(info);
    });
}

Call<std::vector<UserDto>> Client::publicUsers() {
    return jsonCall<std::vector<UserDto>>(request(http::Method::Get, "/Users/Public"), ErrorContext::Generic,
                                          [](const Json& j) {
                                              std::vector<UserDto> users;
                                              if (j.is_array())
                                                  for (const auto& u : j)
                                                      if (u.is_object()) users.push_back(parseUser(u));
                                              return users;
                                          });
}

Call<AuthResult> Client::authenticateByName(std::string username, std::string password) {
    auto self = shared_from_this();
    auto req = request(http::Method::Post, "/Users/AuthenticateByName");
    req.contentType = "application/json";
    req.body = detail::dumpSafe(Json{{"Username", username}, {"Pw", password}});
    return jsonCall<AuthResult>(std::move(req), ErrorContext::Login, [self](const Json& j) -> Result<AuthResult> {
        AuthResult auth = parseAuthResult(j);
        if (auth.accessToken.empty() || auth.user.id.empty())
            return Result<AuthResult>::failure(ApiError::parse("authentication response without token"));
        self->applyAuth(auth);
        return Result<AuthResult>::success(std::move(auth));
    });
}

Call<bool> Client::quickConnectEnabled() {
    return jsonCall<bool>(request(http::Method::Get, "/QuickConnect/Enabled"), ErrorContext::QuickConnect,
                          [](const Json& j) { return j.is_boolean() && j.get<bool>(); });
}

Call<QuickConnectState> Client::quickConnectInitiate() {
    auto self = shared_from_this();
    const bool postFirst = modernApi();
    return Call<QuickConnectState>([self, postFirst](const detail::Exec& exec, Callback<QuickConnectState> done) {
        auto parse = [](const Json& j) -> Result<QuickConnectState> {
            QuickConnectState s = parseQuickConnect(j);
            if (s.secret.empty() || s.code.empty())
                return Result<QuickConnectState>::failure(ApiError::parse("Quick Connect response without code"));
            return Result<QuickConnectState>::success(std::move(s));
        };
        auto attempt = [self, exec, parse, done](bool post, bool allowRetry, auto&& retry) -> void {
            auto req = self->request(post ? http::Method::Post : http::Method::Get, "/QuickConnect/Initiate");
            exec.send(std::move(req), [self, exec, parse, done, post, allowRetry, retry](http::Response resp) {
                if (allowRetry && (resp.status == 404 || resp.status == 405)) return retry(!post, false, retry);
                auto r = self->parseResponse<QuickConnectState>(resp, ErrorContext::QuickConnect, parse);
                if (!r.ok() && r.error.status == 401)
                    r.error = ApiError::make(ErrorKind::Forbidden, "Quick Connect is disabled on this server.",
                                             r.error.status, r.error.detail);
                done(std::move(r));
            });
        };
        attempt(postFirst, true, attempt);
    });
}

Call<QuickConnectState> Client::quickConnectState(std::string secret) {
    return jsonCall<QuickConnectState>(request(http::Method::Get, "/QuickConnect/Connect", {{"secret", secret}}),
                                       ErrorContext::QuickConnect, parseQuickConnect)
        .mapError([](ApiError& e) {
            if (e.kind == ErrorKind::NotFound) e.message = "The Quick Connect code expired. Please start again.";
        });
}

Call<AuthResult> Client::authenticateWithQuickConnect(std::string secret) {
    auto self = shared_from_this();
    auto req = request(http::Method::Post, "/Users/AuthenticateWithQuickConnect");
    req.contentType = "application/json";
    req.body = detail::dumpSafe(Json{{"Secret", secret}});
    return jsonCall<AuthResult>(std::move(req), ErrorContext::QuickConnect,
                                [self](const Json& j) -> Result<AuthResult> {
                                    AuthResult auth = parseAuthResult(j);
                                    if (auth.accessToken.empty() || auth.user.id.empty())
                                        return Result<AuthResult>::failure(
                                            ApiError::parse("Quick Connect authentication without token"));
                                    self->applyAuth(auth);
                                    return Result<AuthResult>::success(std::move(auth));
                                });
}

Call<UserDto> Client::currentUser() {
    auto self = shared_from_this();
    return jsonCall<UserDto>(request(http::Method::Get, "/Users/Me"), ErrorContext::Generic,
                             [self](const Json& j) -> Result<UserDto> {
                                 UserDto u = parseUser(j);
                                 if (u.id.empty()) return Result<UserDto>::failure(ApiError::parse("/Users/Me without Id"));
                                 {
                                     std::lock_guard lock(self->mutex_);
                                     self->user_ = u;
                                 }
                                 return Result<UserDto>::success(std::move(u));
                             });
}

Call<UserDto> Client::validateSession() {
    auto self = shared_from_this();
    return Call<UserDto>([self](const detail::Exec& exec, Callback<UserDto> done) {
        if (self->accessToken().empty()) {
            done(Result<UserDto>::failure(ApiError::make(ErrorKind::Unauthorized, "Please sign in.")));
            return;
        }
        self->publicInfo().run(exec, [self, exec, done](Result<ServerInfo> info) {
            if (!info.ok() && (info.error.isConnectivity() || info.error.kind == ErrorKind::Cancelled))
                return done(Result<UserDto>::failure(info.error));
            if (info.ok()) AccountStore::instance().saveServer(info.value, self->baseUrl());
            self->currentUser().run(exec, [self, done](Result<UserDto> me) {
                const std::string sid = self->serverId();
                if (me.ok()) {
                    AccountStore::instance().saveUser(sid, me.value, self->accessToken());
                    ReportQueue::instance().flush(self);
                } else if (me.error.kind == ErrorKind::Unauthorized) {
                    AccountStore::instance().clearToken(sid, self->userId());
                    std::lock_guard lock(self->mutex_);
                    self->token_.clear();
                }
                done(std::move(me));
            });
        });
    });
}

Call<Unit> Client::logout() {
    auto self = shared_from_this();
    auto req = request(http::Method::Post, "/Sessions/Logout");
    req.timeoutMs = 5000;
    return Call<Unit>([self, req](const detail::Exec& exec, Callback<Unit> done) {
        exec.send(req, [self, done](http::Response resp) {
            if (!resp.ok()) log::info("jellyfin", "logout request failed ({}); clearing locally", resp.describe());
            const std::string sid = self->serverId();
            const std::string uid = self->userId();
            AccountStore::instance().clearToken(sid, uid);
            {
                std::lock_guard lock(self->mutex_);
                self->token_.clear();
                self->user_ = {};
            }
            done(Status::success({}));
        });
    });
}

// --- Current session ---------------------------------------------------------------------------

ClientPtr currentClient() {
    std::lock_guard lock(g_currentMutex);
    return g_current;
}

void setCurrentClient(ClientPtr client) {
    std::lock_guard lock(g_currentMutex);
    g_current = std::move(client);
}

// --- Quick Connect flow ------------------------------------------------------------------------

std::shared_ptr<QuickConnectFlow> QuickConnectFlow::start(ClientPtr client, CodeCallback onCode, DoneCallback onDone,
                                                          double pollSeconds, double timeoutSeconds) {
    auto flow = std::make_shared<QuickConnectFlow>();
    flow->client_ = std::move(client);
    flow->onCode_ = std::move(onCode);
    flow->onDone_ = std::move(onDone);
    flow->pollSeconds_ = std::max(1.0, pollSeconds);
    flow->remaining_ = timeoutSeconds;
    flow->client_->quickConnectInitiate().then(
        [flow](Result<QuickConnectState> r) {
            if (!r.ok()) return flow->finish(Result<AuthResult>::failure(r.error));
            flow->state_ = std::move(r.value);
            if (flow->onCode_) flow->onCode_(flow->state_.code);
            flow->poll();
        },
        flow->token_);
    return flow;
}

void QuickConnectFlow::cancel() {
    token_.cancel();
    done_ = true;
}

void QuickConnectFlow::poll() {
    auto self = shared_from_this();
    main_thread::postDelayed(pollSeconds_, [self] {
        if (self->done_ || self->token_.cancelled()) return;
        self->remaining_ -= self->pollSeconds_;
        if (self->remaining_ <= 0) {
            return self->finish(Result<AuthResult>::failure(
                ApiError::make(ErrorKind::Cancelled, "Quick Connect timed out. Please start again.")));
        }
        self->client_->quickConnectState(self->state_.secret)
            .then(
                [self](Result<QuickConnectState> r) {
                    if (!r.ok()) {
                        if (r.error.isConnectivity()) return self->poll();  // transient: keep trying
                        return self->finish(Result<AuthResult>::failure(r.error));
                    }
                    if (!r->authenticated) return self->poll();
                    self->client_->authenticateWithQuickConnect(self->state_.secret)
                        .then([self](Result<AuthResult> auth) { self->finish(std::move(auth)); }, self->token_);
                },
                self->token_);
    });
}

void QuickConnectFlow::finish(Result<AuthResult> result) {
    if (done_) return;
    done_ = true;
    if (onDone_) onDone_(std::move(result));
}

}  // namespace karafin::jellyfin
