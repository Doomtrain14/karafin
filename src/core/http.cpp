#include "core/http.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <sys/select.h>  // curl/multi.h uses fd_set without including it on newlib
#include <curl/curl.h>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/paths.hpp"
#include "core/settings.hpp"
#include "core/thread.hpp"

namespace karafin::http {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int64_t> g_lastReceiveMs{0};

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

struct Transfer {
    Request req;
    Response resp;
    Callback cb;
    CallbackThread where = CallbackThread::Main;

    CURL* easy = nullptr;
    curl_slist* headerList = nullptr;
    std::string range;
    char errbuf[CURL_ERROR_SIZE] = {};
    Clock::time_point started;
    Clock::time_point lastProgress;
    Progress progress;
    bool aborted = false;
    bool tooLarge = false;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* t = static_cast<Transfer*>(ud);
    const size_t n = size * nmemb;
    if (t->req.cancel.cancelled()) return 0;
    if (t->req.onData) {
        if (!t->req.onData(std::string_view(ptr, n))) {
            t->aborted = true;
            return 0;
        }
    } else {
        if (t->resp.body.size() + n > t->req.maxBodyBytes) {
            t->tooLarge = true;
            return 0;
        }
        t->resp.body.append(ptr, n);
    }
    t->resp.downloadedBytes += n;
    g_lastReceiveMs.store(steadyMs(), std::memory_order_relaxed);
    return n;
}

size_t headerCb(char* buffer, size_t size, size_t nitems, void* ud) {
    auto* t = static_cast<Transfer*>(ud);
    const size_t n = size * nitems;
    std::string_view line(buffer, n);
    if (line.starts_with("HTTP/")) {
        t->resp.headers.clear();  // new response (redirect or 100-continue)
        return n;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return n;
    t->resp.headers.emplace_back(toLower(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
    return n;
}

void reportProgress(Transfer* t, bool force) {
    if (!t->req.onProgress) return;
    const auto now = Clock::now();
    if (!force && now - t->lastProgress < std::chrono::milliseconds(100)) return;
    t->lastProgress = now;
    const double elapsed = std::chrono::duration<double>(now - t->started).count();
    t->progress.bytesPerSecond = elapsed > 0 ? static_cast<double>(t->progress.downloaded) / elapsed : 0;
    t->req.onProgress(t->progress);
}

int xferInfoCb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* t = static_cast<Transfer*>(ud);
    if (t->req.cancel.cancelled()) return 1;
    t->progress.downloaded = static_cast<uint64_t>(dlnow);
    t->progress.total = static_cast<uint64_t>(dltotal);
    t->progress.uploaded = static_cast<uint64_t>(ulnow);
    t->progress.uploadTotal = static_cast<uint64_t>(ultotal);
    reportProgress(t, false);
    return 0;
}

Error mapCurlError(CURLcode rc) {
    switch (rc) {
        case CURLE_OK: return Error::None;
        case CURLE_OPERATION_TIMEDOUT: return Error::Timeout;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY: return Error::Resolve;
        case CURLE_COULDNT_CONNECT: return Error::Connect;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR: return Error::Tls;
        case CURLE_ABORTED_BY_CALLBACK: return Error::Cancelled;
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_HTTP2:
        case CURLE_HTTP2_STREAM: return Error::Network;
        default: return Error::Other;
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------------

struct Client::Impl {
    std::atomic<bool> running{false};
    std::atomic<bool> verifyTls{true};
    mutable std::mutex configMutex;
    std::string userAgent = "Karafin/" KARAFIN_VERSION " (Nintendo Switch)";
    std::string caBundle;

    CURLM* multi = nullptr;
    CURLSH* share = nullptr;
    std::mutex shareLocks[CURL_LOCK_DATA_LAST];

    // Also guards `multi` against curl_multi_wakeup() from other threads while resetMulti()
    // replaces it.
    std::mutex queueMutex;
    std::deque<std::unique_ptr<Transfer>> incoming;
    std::unordered_map<CURL*, std::unique_ptr<Transfer>> active;  // http thread only
    std::deque<std::unique_ptr<Transfer>> deferred;               // background, waiting for a slot; http thread only
    std::atomic<size_t> activeCount{0};
    std::atomic<bool> resetRequested{false};

    void wakeup() {
        std::lock_guard lock(queueMutex);
        if (multi) curl_multi_wakeup(multi);
    }

    static CURLM* createMulti() {
        CURLM* m = curl_multi_init();
        curl_multi_setopt(m, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
        curl_multi_setopt(m, CURLMOPT_MAX_TOTAL_CONNECTIONS, 16L);
        // Idle pooled connections keep their socket buffers; with downloads running, a large pool
        // can exhaust the BSD transfer memory and make new connections fail.
        curl_multi_setopt(m, CURLMOPT_MAXCONNECTS, 8L);
        return m;
    }

    // http thread only.
    void resetMulti() {
        size_t failed = 0;
        for (auto& [easy, t] : active) {
            curl_multi_remove_handle(multi, easy);
            finalize(*t, CURLE_RECV_ERROR);
            if (t->resp.error == Error::Network) t->resp.errorMessage = "connection reset after sleep";
            dispatch(std::move(t));
            ++failed;
        }
        active.clear();
        std::lock_guard lock(queueMutex);
        curl_multi_cleanup(multi);
        multi = createMulti();
        log::info("http", "connection pool reset ({} transfers failed)", failed);
    }

    Thread thread;

    static void lockCb(CURL*, curl_lock_data data, curl_lock_access, void* ud) {
        static_cast<Impl*>(ud)->shareLocks[data].lock();
    }
    static void unlockCb(CURL*, curl_lock_data data, void* ud) { static_cast<Impl*>(ud)->shareLocks[data].unlock(); }

    void loadCaBundle() {
        caBundle.clear();
        if (auto romfsCa = fs::readFile(paths::romfs("certs/cacert.pem"))) caBundle = std::move(*romfsCa);
        if (auto userCa = fs::readFile(paths::dataFile("cacert.pem"))) {
            caBundle += "\n";
            caBundle += *userCa;
            log::info("http", "loaded user CA bundle ({} bytes)", userCa->size());
        }
        if (caBundle.empty()) log::warn("http", "no CA bundle found; HTTPS verification will fail");
    }

    void configure(Transfer& t) {
        CURL* e = t.easy;
        const Request& r = t.req;
        t.started = t.lastProgress = Clock::now();

        curl_easy_setopt(e, CURLOPT_URL, r.url.c_str());
        curl_easy_setopt(e, CURLOPT_PRIVATE, &t);
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t.errbuf);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 64L * 1024);
        curl_easy_setopt(e, CURLOPT_SHARE, share);
        {
            std::lock_guard lock(configMutex);
            curl_easy_setopt(e, CURLOPT_USERAGENT, userAgent.c_str());
        }

        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &writeCb);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, &headerCb);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, &xferInfoCb);
        curl_easy_setopt(e, CURLOPT_XFERINFODATA, &t);
        curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);

        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, r.followRedirects ? 1L : 0L);
        curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, r.connectTimeoutMs > 0 ? r.connectTimeoutMs : kDefaultConnectTimeoutMs);
        // Streaming (sink/progress) transfers such as downloads may legitimately run for hours, so
        // an unset total timeout stays unlimited for them; the low-speed limit below still aborts
        // a stalled connection. Buffered requests always get a total timeout.
        const bool streaming = static_cast<bool>(r.onData) || static_cast<bool>(r.onProgress);
        const long totalMs = r.timeoutMs > 0 ? r.timeoutMs : (streaming ? 0L : kDefaultBufferedTimeoutMs);
        if (totalMs > 0) curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, totalMs);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, r.lowSpeedLimit > 0 ? r.lowSpeedLimit : 1L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, r.lowSpeedTimeSec > 0 ? r.lowSpeedTimeSec : kDefaultLowSpeedTimeSec);
        if (r.acceptCompressed && !r.rangeFrom) curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");

        const bool verify = r.verifyTls && verifyTls.load();
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
        if (verify && !caBundle.empty()) {
            curl_blob blob{caBundle.data(), caBundle.size(), CURL_BLOB_NOCOPY};
            curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &blob);
        }

        if (r.rangeFrom) {
            t.range = std::to_string(*r.rangeFrom) + "-";
            if (r.rangeTo) t.range += std::to_string(*r.rangeTo);
            curl_easy_setopt(e, CURLOPT_RANGE, t.range.c_str());
        }

        switch (r.method) {
            case Method::Get: break;
            case Method::Head: curl_easy_setopt(e, CURLOPT_NOBODY, 1L); break;
            case Method::Post:
                curl_easy_setopt(e, CURLOPT_POST, 1L);
                curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
                curl_easy_setopt(e, CURLOPT_POSTFIELDS, r.body.c_str());
                break;
            case Method::Put:
            case Method::Delete:
                curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, r.method == Method::Put ? "PUT" : "DELETE");
                if (!r.body.empty() || r.method == Method::Put) {
                    curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
                    curl_easy_setopt(e, CURLOPT_POSTFIELDS, r.body.c_str());
                }
                break;
        }

        curl_slist* list = nullptr;
        for (const auto& [k, v] : r.headers) list = curl_slist_append(list, (k + ": " + v).c_str());
        if (!r.contentType.empty()) list = curl_slist_append(list, ("Content-Type: " + r.contentType).c_str());
        list = curl_slist_append(list, "Expect:");
        t.headerList = list;
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, list);
    }

    void finalize(Transfer& t, CURLcode rc) {
        long status = 0;
        curl_easy_getinfo(t.easy, CURLINFO_RESPONSE_CODE, &status);
        t.resp.status = status;
        char* effective = nullptr;
        if (curl_easy_getinfo(t.easy, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective)
            t.resp.effectiveUrl = effective;
        curl_off_t totalUs = 0;
        curl_easy_getinfo(t.easy, CURLINFO_TOTAL_TIME_T, &totalUs);
        t.resp.totalSeconds = static_cast<double>(totalUs) / 1e6;

        if (t.req.cancel.cancelled()) {
            t.resp.error = Error::Cancelled;
        } else if (t.aborted) {
            t.resp.error = Error::Aborted;
        } else if (t.tooLarge) {
            t.resp.error = Error::TooLarge;
        } else {
            t.resp.error = mapCurlError(rc);
        }
        if (t.resp.error != Error::None) {
            t.resp.errorMessage = t.errbuf[0] ? t.errbuf : curl_easy_strerror(rc);
        }
        reportProgress(&t, true);

        curl_slist_free_all(t.headerList);
        t.headerList = nullptr;
        curl_easy_cleanup(t.easy);
        t.easy = nullptr;
    }

    void dispatch(std::unique_ptr<Transfer> t) {
        if (!t->cb) return;
        if (t->where == CallbackThread::Http) {
            t->cb(std::move(t->resp));
            return;
        }
        std::shared_ptr<Transfer> shared(std::move(t));
        main_thread::post([shared] { shared->cb(std::move(shared->resp)); });
    }

    // http thread only.
    void begin(std::unique_ptr<Transfer> t) {
        t->easy = curl_easy_init();
        if (!t->easy) {
            t->resp.error = Error::Other;
            t->resp.errorMessage = "curl_easy_init failed";
            dispatch(std::move(t));
            return;
        }
        configure(*t);
        curl_multi_add_handle(multi, t->easy);
        CURL* key = t->easy;
        active.emplace(key, std::move(t));
    }

    // Starts deferred background transfers while fewer than kMaxBackground are running. Kept
    // below CURLMOPT_MAX_HOST_CONNECTIONS so foreground requests to the same server never wait.
    void admitBackground() {
        static constexpr size_t kMaxBackground = 4;
        size_t running = 0;
        for (const auto& [easy, t] : active) running += t->req.background ? 1 : 0;
        while (running < kMaxBackground && !deferred.empty()) {
            auto t = std::move(deferred.front());
            deferred.pop_front();
            begin(std::move(t));
            ++running;
        }
    }

    void loop() {
        while (running.load()) {
            if (resetRequested.exchange(false)) resetMulti();
            std::deque<std::unique_ptr<Transfer>> batch;
            {
                std::lock_guard lock(queueMutex);
                batch.swap(incoming);
            }
            for (auto& t : batch) {
                if (t->req.background) deferred.push_back(std::move(t));
                else begin(std::move(t));
            }

            for (auto it = deferred.begin(); it != deferred.end();) {
                if ((*it)->req.cancel.cancelled()) {
                    (*it)->resp.error = Error::Cancelled;
                    (*it)->resp.errorMessage = "cancelled";
                    dispatch(std::move(*it));
                    it = deferred.erase(it);
                } else {
                    ++it;
                }
            }
            admitBackground();

            for (auto it = active.begin(); it != active.end();) {
                if (it->second->req.cancel.cancelled()) {
                    curl_multi_remove_handle(multi, it->first);
                    finalize(*it->second, CURLE_ABORTED_BY_CALLBACK);
                    dispatch(std::move(it->second));
                    it = active.erase(it);
                } else {
                    ++it;
                }
            }

            int stillRunning = 0;
            curl_multi_perform(multi, &stillRunning);

            int queued = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi, &queued)) {
                if (msg->msg != CURLMSG_DONE) continue;
                CURL* easy = msg->easy_handle;
                const CURLcode rc = msg->data.result;
                auto it = active.find(easy);
                curl_multi_remove_handle(multi, easy);
                if (it == active.end()) continue;
                auto t = std::move(it->second);
                active.erase(it);
                finalize(*t, rc);
                dispatch(std::move(t));
            }
            admitBackground();
            activeCount.store(active.size() + deferred.size());

            curl_multi_poll(multi, nullptr, 0, active.empty() ? 1000 : 50, nullptr);
        }

        for (auto& t : deferred) stopped.push_back(std::move(t));
        deferred.clear();

        // Fail in-flight transfers as cancelled; stop() delivers them after join().
        for (auto& [easy, t] : active) {
            curl_multi_remove_handle(multi, easy);
            finalize(*t, CURLE_ABORTED_BY_CALLBACK);
            stopped.push_back(std::move(t));
        }
        active.clear();
        activeCount.store(0);
    }

    // Filled by the http thread as it exits; read by stop() after join().
    std::vector<std::unique_ptr<Transfer>> stopped;
};

// ------------------------------------------------------------------------------------------------

Client& Client::instance() {
    static Client c;
    return c;
}

void Client::start() {
    if (impl_ && impl_->running.load()) return;
    if (!impl_) impl_ = new Impl();

    curl_global_init(CURL_GLOBAL_DEFAULT);
    const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    // Without async DNS a lookup cannot be cancelled, so a stalled resolver can delay shutdown.
    log::info("http", "libcurl {} ({}, async DNS {})", v->version, v->ssl_version ? v->ssl_version : "no TLS",
              (v->features & CURL_VERSION_ASYNCHDNS) ? "yes" : "no");

    impl_->verifyTls.store(Settings::instance().get("http.verify_tls", true));
    impl_->loadCaBundle();

    impl_->share = curl_share_init();
    curl_share_setopt(impl_->share, CURLSHOPT_LOCKFUNC, &Impl::lockCb);
    curl_share_setopt(impl_->share, CURLSHOPT_UNLOCKFUNC, &Impl::unlockCb);
    curl_share_setopt(impl_->share, CURLSHOPT_USERDATA, impl_);
    curl_share_setopt(impl_->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(impl_->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

    impl_->multi = Impl::createMulti();

    impl_->running.store(true);
    ThreadOptions opts;
    opts.name = "http";
    opts.stackSize = 512 * 1024;
    opts.priority = kPriorityIo;
    opts.core = 2;
    impl_->thread = Thread(std::move(opts), [impl = impl_] { impl->loop(); });
}

void Client::stop() {
    if (!impl_ || !impl_->running.load()) return;
    impl_->running.store(false);
    impl_->wakeup();
    impl_->thread.join();
    std::vector<std::unique_ptr<Transfer>> pending = std::move(impl_->stopped);
    impl_->stopped.clear();
    {
        std::lock_guard lock(impl_->queueMutex);
        for (auto& t : impl_->incoming) pending.push_back(std::move(t));
        impl_->incoming.clear();
        curl_multi_cleanup(impl_->multi);
        impl_->multi = nullptr;
    }
    // Complete outside the lock: callbacks may call send(), which takes queueMutex.
    for (auto& t : pending) {
        t->resp.error = Error::Cancelled;
        t->resp.errorMessage = "http client stopped";
        impl_->dispatch(std::move(t));
    }
    if (!pending.empty()) log::info("http", "cancelled {} pending requests", pending.size());
    curl_share_cleanup(impl_->share);
    impl_->share = nullptr;
    curl_global_cleanup();
    log::info("http", "stopped");
}

void Client::setUserAgent(std::string userAgent) {
    if (!impl_) impl_ = new Impl();
    std::lock_guard lock(impl_->configMutex);
    impl_->userAgent = std::move(userAgent);
}

std::string Client::userAgent() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->configMutex);
    return impl_->userAgent;
}

void Client::setVerifyTls(bool verify) {
    if (!impl_) impl_ = new Impl();
    impl_->verifyTls.store(verify);
}

void Client::send(Request req, Callback cb, CallbackThread where) {
    auto t = std::make_unique<Transfer>();
    t->req = std::move(req);
    t->cb = std::move(cb);
    t->where = where;
    if (!impl_ || !impl_->running.load()) {
        t->resp.error = Error::Other;
        t->resp.errorMessage = "http client not running";
        if (!t->cb) return;
        if (where == CallbackThread::Main && !main_thread::isCurrent()) {
            std::shared_ptr<Transfer> shared(std::move(t));
            main_thread::post([shared] { shared->cb(std::move(shared->resp)); });
        } else {
            t->cb(std::move(t->resp));
        }
        return;
    }
    {
        // Re-checked under the lock so a request racing stop() is either drained by stop() or
        // completed below, never stranded in `incoming`.
        std::lock_guard lock(impl_->queueMutex);
        if (impl_->running.load()) {
            impl_->incoming.push_back(std::move(t));
            if (impl_->multi) curl_multi_wakeup(impl_->multi);
            return;
        }
    }
    t->resp.error = Error::Cancelled;
    t->resp.errorMessage = "http client stopped";
    impl_->dispatch(std::move(t));
}

void Client::resetConnections() {
    if (!impl_ || !impl_->running.load()) return;
    impl_->resetRequested.store(true);
    impl_->wakeup();
}

std::future<Response> Client::sendFuture(Request req) {
    auto promise = std::make_shared<std::promise<Response>>();
    auto fut = promise->get_future();
    send(std::move(req), [promise](Response&& r) { promise->set_value(std::move(r)); }, CallbackThread::Http);
    return fut;
}

Response Client::perform(Request req) {
    if (main_thread::isCurrent()) log::warn("http", "perform() on the main thread stalls the UI: {}", req.url);
    Transfer t;
    t.req = std::move(req);
    if (!impl_ || !impl_->running.load()) {
        t.resp.error = Error::Other;
        t.resp.errorMessage = "http client not running";
        return std::move(t.resp);
    }
    t.easy = curl_easy_init();
    if (!t.easy) {
        t.resp.error = Error::Other;
        t.resp.errorMessage = "curl_easy_init failed";
        return std::move(t.resp);
    }
    impl_->configure(t);
    const CURLcode rc = curl_easy_perform(t.easy);
    impl_->finalize(t, rc);
    return std::move(t.resp);
}

void Client::get(std::string url, Headers headers, Callback cb) {
    Request r;
    r.url = std::move(url);
    r.headers = std::move(headers);
    send(std::move(r), std::move(cb));
}

void Client::postJson(std::string url, const nlohmann::json& body, Headers headers, Callback cb) {
    Request r;
    r.method = Method::Post;
    r.url = std::move(url);
    r.headers = std::move(headers);
    r.body = body.dump();
    r.contentType = "application/json";
    send(std::move(r), std::move(cb));
}

size_t Client::activeTransfers() const { return impl_ ? impl_->activeCount.load() : 0; }

int64_t msSinceLastReceive() {
    const int64_t last = g_lastReceiveMs.load(std::memory_order_relaxed);
    return last == 0 ? -1 : steadyMs() - last;
}

// ------------------------------------------------------------------------------------------------

std::string_view errorName(Error e) {
    switch (e) {
        case Error::None: return "None";
        case Error::Cancelled: return "Cancelled";
        case Error::Timeout: return "Timeout";
        case Error::Resolve: return "Resolve";
        case Error::Connect: return "Connect";
        case Error::Tls: return "Tls";
        case Error::TooLarge: return "TooLarge";
        case Error::Aborted: return "Aborted";
        case Error::Network: return "Network";
        case Error::Other: return "Other";
    }
    return "?";
}

const std::string* Response::header(std::string_view name) const {
    const std::string lower = toLower(name);
    for (const auto& [k, v] : headers)
        if (k == lower) return &v;
    return nullptr;
}

std::optional<uint64_t> Response::contentLength() const {
    const std::string* v = header("content-length");
    if (!v || v->empty()) return std::nullopt;
    try {
        return static_cast<uint64_t>(std::stoull(*v));
    } catch (...) {
        return std::nullopt;
    }
}

nlohmann::json Response::json() const { return nlohmann::json::parse(body, nullptr, false); }

std::string Response::describe() const {
    if (error != Error::None) return std::string(errorName(error)) + ": " + errorMessage;
    return "HTTP " + std::to_string(status);
}

std::string urlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string buildQuery(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (const auto& [k, v] : params) {
        if (!out.empty()) out += '&';
        out += urlEncode(k);
        out += '=';
        out += urlEncode(v);
    }
    return out;
}

std::string joinUrl(std::string_view base, std::string_view path) {
    std::string out(base);
    while (!out.empty() && out.back() == '/') out.pop_back();
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    out += '/';
    out += path;
    return out;
}

}  // namespace karafin::http
