#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cancel.hpp"

// HTTP(S) client on libcurl + mbedtls.
//
// Two ways to run a request:
//   * Client::send(req, cb)  - async; runs on the shared "http" thread (curl multi, many concurrent
//                              transfers). The callback runs on the main thread by default.
//   * Client::perform(req)   - blocking on the calling thread. For dedicated worker threads only
//                              (e.g. the download manager); never call it on the main thread.
//
// TLS: verified against the Mozilla CA bundle in romfs:/certs/cacert.pem, plus an optional user
// bundle at sdmc:/switch/karafin/cacert.pem (for self-signed / private CA servers). Verification
// can be disabled per request (Request::verifyTls) or globally (setting "http.verify_tls").
namespace karafin::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

enum class Method { Get, Head, Post, Put, Delete };

enum class Error {
    None,
    Cancelled,  // CancelToken fired
    Timeout,
    Resolve,    // DNS failure
    Connect,    // TCP connect refused / unreachable
    Tls,        // handshake or certificate failure
    TooLarge,   // buffered body exceeded Request::maxBodyBytes
    Aborted,    // Request::onData returned false
    Network,    // connection dropped mid-transfer, etc.
    Other,
};

std::string_view errorName(Error e);

// Safe defaults applied when a Request leaves the corresponding limit unset (<= 0).
inline constexpr long kDefaultConnectTimeoutMs = 10000;
inline constexpr long kDefaultBufferedTimeoutMs = 120000;
inline constexpr long kDefaultLowSpeedTimeSec = 30;

struct Progress {
    uint64_t downloaded = 0;
    uint64_t total = 0;  // 0 if unknown; excludes the Range offset
    uint64_t uploaded = 0;
    uint64_t uploadTotal = 0;
    double bytesPerSecond = 0;  // average download speed of this transfer
};

struct Request {
    Method method = Method::Get;
    std::string url;
    Headers headers;
    std::string body;         // POST/PUT payload
    std::string contentType;  // sets Content-Type when non-empty

    // Byte range (HTTP Range). rangeFrom only -> "bytes=N-" (resume). Both -> inclusive range.
    std::optional<uint64_t> rangeFrom;
    std::optional<uint64_t> rangeTo;

    // Whole-transfer limit. <= 0 means "no explicit limit": streaming requests (onData or
    // onProgress set, e.g. downloads) then get no total timeout, buffered requests get
    // kDefaultBufferedTimeoutMs. Stall detection (low-speed limit) always applies.
    long timeoutMs = 30000;
    long connectTimeoutMs = 10000;  // <= 0 -> kDefaultConnectTimeoutMs
    long lowSpeedLimit = 1;     // bytes/s; abort if slower than this... (<= 0 -> 1)
    long lowSpeedTimeSec = 30;  // ...for this many seconds (<= 0 -> kDefaultLowSpeedTimeSec)
    bool followRedirects = true;
    bool verifyTls = true;
    bool acceptCompressed = true;  // Accept-Encoding: gzip/deflate (disable for Range downloads)
    size_t maxBodyBytes = 32u * 1024 * 1024;
    // Bulk traffic such as artwork. Only a few background transfers run at once, so API calls
    // always find a free connection instead of timing out behind a screenful of posters.
    bool background = false;

    // Streaming sink. When set, the body is not buffered in Response::body. Runs on the
    // transferring thread. Return false to abort (Error::Aborted).
    std::function<bool(std::string_view chunk)> onData;
    // Called on the transferring thread, throttled to ~10 Hz plus a final call.
    std::function<void(const Progress&)> onProgress;

    CancelToken cancel;
};

struct Response {
    long status = 0;
    Error error = Error::None;
    std::string errorMessage;
    std::string body;
    Headers headers;  // names lower-cased; final response only (after redirects)
    std::string effectiveUrl;
    uint64_t downloadedBytes = 0;
    double totalSeconds = 0;

    bool ok() const { return error == Error::None && status >= 200 && status < 300; }
    bool cancelled() const { return error == Error::Cancelled; }
    const std::string* header(std::string_view name) const;  // case-insensitive
    std::optional<uint64_t> contentLength() const;
    // Parses the body; returns a discarded value (json::is_discarded()) on invalid JSON.
    nlohmann::json json() const;
    // Short human readable summary for logs / error UI ("HTTP 401", "Timeout: ...").
    std::string describe() const;
};

using Callback = std::function<void(Response&&)>;

enum class CallbackThread { Main, Http };

class Client {
public:
    static Client& instance();

    // Started/stopped by App. Loads the CA bundle and spawns the "http" thread.
    void start();
    void stop();

    void setUserAgent(std::string userAgent);
    std::string userAgent() const;
    // Global switch, also read from setting "http.verify_tls" at start().
    void setVerifyTls(bool verify);

    // Async. The callback runs exactly once, on the main thread (default) or directly on the http
    // thread (keep it tiny - it blocks all other transfers); cancelled requests complete with
    // Error::Cancelled. stop() completes every queued and in-flight request with Error::Cancelled
    // ("http client stopped"): Http-thread callbacks (and sendFuture futures) run on the thread
    // calling stop(); main-thread callbacks are posted and are dropped if the main queue is
    // cleared afterwards (as App does at shutdown).
    void send(Request req, Callback cb, CallbackThread where = CallbackThread::Main);
    // Async with a future (useful from worker threads).
    std::future<Response> sendFuture(Request req);

    // Blocking on the calling thread. Not for the main thread.
    Response perform(Request req);

    // Convenience wrappers around send().
    void get(std::string url, Headers headers, Callback cb);
    void postJson(std::string url, const nlohmann::json& body, Headers headers, Callback cb);

    size_t activeTransfers() const;

    // Drops every pooled keep-alive connection and fails in-flight async transfers with
    // Error::Network. Call after the console wakes from sleep: reusing a pre-sleep socket can
    // otherwise stall a request until the low-speed timeout. Any thread.
    void resetConnections();

private:
    Client() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// Percent-encodes everything except RFC 3986 unreserved characters.
std::string urlEncode(std::string_view s);
// "a=1&b=x%20y" (values encoded). Empty values are kept.
std::string buildQuery(const std::vector<std::pair<std::string, std::string>>& params);
// Joins base and path with exactly one '/'.
std::string joinUrl(std::string_view base, std::string_view path);

}  // namespace karafin::http
