#include "gfx/image.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>

#include <dirent.h>
#include <sys/stat.h>

#include "core/app.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/paths.hpp"
#include "core/perf.hpp"
#include "core/platform.hpp"
#include "core/settings.hpp"
#include "core/thread_pool.hpp"

namespace karafin::gfx {

struct ImageCache::Result {
    std::string key;
    uint64_t generation = 0;
    bool ok = false;
    bool cancelled = false;  // nobody wanted it any more: back to Idle, not Failed
    bool transient = false;  // network / server error: worth retrying later
    Pixmap pixmap;
    Color dominant;
    std::string error;
    // Upload in progress: storage allocated, rows [0, rowsUploaded) copied.
    TexturePtr texture;
    int rowsUploaded = 0;
    // Bytes of `pixmap` counted against the pending-upload limit; released when destroyed.
    size_t charged = 0;

    Result() = default;
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    ~Result();
};

namespace {

constexpr int kMaxRetries = 3;
constexpr double kRetryDelay = 8.0;      // seconds, times the failure count
constexpr uint64_t kCancelAfterFrames = 30;
// Keep the CPU boost a while after the last load: every boost on/off is a clock change (DVFS),
// and flapping it while browsing costs more than it saves.
constexpr uint64_t kBoostLingerFrames = 180;
// Largest single glTexSubImage2D; the time budget is checked between bands.
constexpr size_t kUploadBandBytes = 512u * 1024;

// The boost is acquired/released on a worker: clkrst calls are IPC round trips that do not
// belong on the main thread. Tasks may run in any order, so each one just makes the boost match
// the latest wish.
std::atomic<bool> g_boostWanted{false};
std::mutex g_boostMutex;
std::unique_ptr<platform::CpuBoost> g_boost;

void syncBoost() {
    std::lock_guard lock(g_boostMutex);
    const bool want = g_boostWanted.load();
    if (want && !g_boost) g_boost = std::make_unique<platform::CpuBoost>();
    else if (!want && g_boost) g_boost.reset();
}

struct DecodeSpec {
    int width = 0, height = 0;
    ImageFit fit = ImageFit::Cover;
    float blur = 0;
};

uint64_t fnv1a(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool isLocal(const std::string& url) {
    return url.starts_with("romfs:") || url.starts_with("sdmc:") || url.starts_with("/");
}

std::string imageDir() { return paths::cacheDir() + "/img"; }

std::string diskPath(const std::string& diskKey) {
    return imageDir() + "/" + diskKey.substr(0, 1) + "/" + diskKey + ".bin";
}

double now() { return App::instance().time(); }

// Several requests (different decode sizes) can share one disk entry and write it through the
// same "<path>.tmp", so concurrent writers of the same path would corrupt each other.
std::mutex g_writeMutex;
std::unordered_set<std::string> g_writing;

// Disk entries: a 24-byte header (magic, version, payload length, payload checksum) followed by
// the original bytes, so a file cut short or garbled (crash, SD error) is detected even when the
// remaining bytes would still decode. Files without a valid header (incl. the old headerless
// format) are deleted and refetched.
constexpr char kDiskMagic[4] = {'K', 'F', 'I', 'C'};
constexpr uint32_t kDiskVersion = 1;
constexpr size_t kDiskHeaderBytes = 24;

uint64_t payloadChecksum(std::string_view s) {
    // FNV-1a over 8-byte words (with a fold so high bits reach the low ones): cheap on big files.
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h = 1469598103934665603ull ^ s.size();
    size_t i = 0;
    for (; i + 8 <= s.size(); i += 8) {
        uint64_t w;
        std::memcpy(&w, s.data() + i, 8);
        h = (h ^ w) * kPrime;
        h ^= h >> 32;
    }
    for (; i < s.size(); ++i) h = (h ^ static_cast<uint8_t>(s[i])) * kPrime;
    return h;
}

void putLe(char* out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

uint64_t getLe(const char* in, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(in[i])) << (8 * i);
    return v;
}

// The payload of a valid disk entry, or nullopt.
std::optional<std::string_view> diskPayload(const std::string& file) {
    if (file.size() < kDiskHeaderBytes || std::memcmp(file.data(), kDiskMagic, 4) != 0) return std::nullopt;
    const char* hdr = file.data();
    if (getLe(hdr + 4, 4) != kDiskVersion) return std::nullopt;
    const uint64_t length = getLe(hdr + 8, 8);
    if (length == 0 || length != file.size() - kDiskHeaderBytes) return std::nullopt;
    const std::string_view payload(file.data() + kDiskHeaderBytes, file.size() - kDiskHeaderBytes);
    if (getLe(hdr + 16, 8) != payloadChecksum(payload)) return std::nullopt;
    return payload;
}

// Header + payload into "<path>.tmp", then renamed over <path>. Horizon's rename fails when the
// target exists, so it is removed first; a crash in between only loses this cache entry.
bool writeDiskFile(const std::string& path, std::string_view payload) {
    char hdr[kDiskHeaderBytes];
    std::memcpy(hdr, kDiskMagic, 4);
    putLe(hdr + 4, kDiskVersion, 4);
    putLe(hdr + 8, payload.size(), 8);
    putLe(hdr + 16, payloadChecksum(payload), 8);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
              std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) {
        std::remove(path.c_str());
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) std::remove(tmp.c_str());
    return ok;
}

// Deletes the oldest cache files until the directory fits in `budget` bytes. Returns the bytes left.
uint64_t trimDiskCache(const std::string& root, size_t budget) {
    struct Entry {
        std::string path;
        size_t size;
        time_t mtime;
    };
    std::vector<Entry> files;
    uint64_t total = 0;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        const std::string dir = root + "/" + hex[i];
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string p = dir + "/" + e->d_name;
            struct stat st{};
            if (stat(p.c_str(), &st) != 0) continue;
            files.push_back({std::move(p), static_cast<size_t>(st.st_size), st.st_mtime});
            total += static_cast<size_t>(st.st_size);
        }
        closedir(d);
    }
    if (total <= budget) {
        log::debug("image", "disk cache {} files, {} KiB", files.size(), total / 1024);
        return total;
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
    size_t removed = 0;
    for (const Entry& f : files) {
        if (total <= budget * 9 / 10) break;
        if (std::remove(f.path.c_str()) == 0) {
            total -= f.size;
            ++removed;
        }
    }
    log::info("image", "disk cache trimmed {} files, now {} KiB", removed, total / 1024);
    return total;
}

// Running estimate of the disk cache size, so a launch does not have to stat every file. The
// full scan (trimDiskCache) runs when the estimate goes over the budget, or when the last scan is
// more than a day old (corrects drift, e.g. writes not saved before a crash). The estimate and
// the time of the last scan persist in "<root>/state".
constexpr int64_t kDiskScanInterval = 24 * 60 * 60;
constexpr uint32_t kDiskStateSaveEvery = 32;  // written entries between state saves

std::atomic<uint64_t> g_diskBytes{0};
std::atomic<size_t> g_diskBudget{0};
std::atomic<int64_t> g_diskScanTime{0};
std::atomic<uint32_t> g_diskUnsaved{0};
std::atomic<bool> g_diskScanning{false};
std::mutex g_diskStateMutex;

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

std::string diskStatePath() { return imageDir() + "/state"; }

void saveDiskState() {
    std::lock_guard lock(g_diskStateMutex);
    g_diskUnsaved.store(0);
    fs::writeFileAtomic(diskStatePath(), std::format("{} {}\n", g_diskScanTime.load(), g_diskBytes.load()));
}

void scanDiskCache() {
    if (g_diskScanning.exchange(true)) return;
    g_diskBytes.store(trimDiskCache(imageDir(), g_diskBudget.load()));
    g_diskScanTime.store(unixNow());
    saveDiskState();
    g_diskScanning.store(false);
}

// Startup: trusts the saved estimate when it is recent and within budget, otherwise scans.
void loadDiskState() {
    long long scanned = 0;
    unsigned long long bytes = 0;
    const int64_t t = unixNow();
    const auto state = fs::readFileAtomic(diskStatePath());
    if (state && std::sscanf(state->c_str(), "%lld %llu", &scanned, &bytes) == 2 && scanned <= t &&
        t - scanned < kDiskScanInterval && bytes <= g_diskBudget.load()) {
        g_diskBytes.fetch_add(bytes);
        g_diskScanTime.store(scanned);
        log::debug("image", "disk cache ~{} KiB (scanned {} s ago)", bytes / 1024, t - scanned);
        return;
    }
    scanDiskCache();
}

void noteDiskWrite(uint64_t bytes) {
    const uint64_t total = g_diskBytes.fetch_add(bytes) + bytes;
    if (total > g_diskBudget.load()) {
        if (!g_diskScanning.load())
            workers().post([] {
                if (g_diskBytes.load() > g_diskBudget.load()) scanDiskCache();
            });
    } else if (g_diskUnsaved.fetch_add(1) + 1 >= kDiskStateSaveEvery) {
        saveDiskState();
    }
}

void writeDiskEntry(const std::string& path, std::string_view bytes) {
    if (bytes.empty()) return;
    {
        std::lock_guard lock(g_writeMutex);
        if (!g_writing.insert(path).second) return;
    }
    if (!fs::exists(path)) {
        fs::ensureDir(fs::parentDir(path));
        if (writeDiskFile(path, bytes)) noteDiskWrite(kDiskHeaderBytes + bytes.size());
        else log::debug("image", "disk cache write failed: {}", path);
    }
    std::lock_guard lock(g_writeMutex);
    g_writing.erase(path);
}

// Decoded pixels waiting for (or in) a texture upload, from decode until the Result is destroyed
// (uploaded, dropped as stale, or discarded at shutdown). Workers wait before decoding while it is
// over the limit, so a slow main thread cannot accumulate unbounded decoded images.
constexpr size_t kMaxPendingBytes = 32u * 1024 * 1024;
// Longest a worker waits for room; afterwards the load is handed back as cancelled (Idle, reloads
// when drawn again) so pool threads are never parked indefinitely.
constexpr auto kPendingWaitMax = std::chrono::seconds(3);
constexpr auto kPendingPoll = std::chrono::milliseconds(50);

std::mutex g_pendingMutex;
std::condition_variable g_pendingCv;
size_t g_pendingBytes = 0;
bool g_pendingStop = false;  // ImageCache::shutdown(): wake and abandon every wait

void chargePending(size_t bytes) {
    std::lock_guard lock(g_pendingMutex);
    g_pendingBytes += bytes;
}

void releasePending(size_t bytes) {
    if (bytes == 0) return;
    {
        std::lock_guard lock(g_pendingMutex);
        g_pendingBytes -= std::min(g_pendingBytes, bytes);
    }
    g_pendingCv.notify_all();
}

void setPendingStopped(bool stopped) {
    {
        std::lock_guard lock(g_pendingMutex);
        g_pendingStop = stopped;
    }
    g_pendingCv.notify_all();
}

// False when the load should be abandoned (cancelled, shutting down or waited too long).
bool waitForPendingRoom(const CancelToken& cancel) {
    const auto deadline = std::chrono::steady_clock::now() + kPendingWaitMax;
    std::unique_lock lock(g_pendingMutex);
    while (!g_pendingStop && g_pendingBytes >= kMaxPendingBytes) {
        if (cancel.cancelled()) return false;
        const auto t = std::chrono::steady_clock::now();
        if (t >= deadline) return false;
        g_pendingCv.wait_until(lock, std::min(deadline, t + kPendingPoll));
    }
    return !g_pendingStop && !cancel.cancelled();
}

}  // namespace

ImageCache::Result::~Result() { releasePending(charged); }

namespace {
// Memory key for log::step lines, without the URL's query (it can carry an ApiKey).
std::string traceName(std::string_view key) {
    const size_t q = key.find('?');
    if (q == std::string_view::npos) return std::string(key);
    const size_t bar = key.find('|', q);
    std::string out(key.substr(0, q));
    if (bar != std::string_view::npos) out.append(key.substr(bar));
    return out;
}
}  // namespace

struct ImageCacheAccess {
    using Result = ImageCache::Result;

    static std::shared_ptr<Result> decode(std::string key, uint64_t gen, std::string_view bytes,
                                          const DecodeSpec& spec, const CancelToken& cancel) {
        auto res = std::make_shared<Result>();
        res->key = std::move(key);
        res->generation = gen;
        // Back to Idle when abandoned; the image reloads if it is drawn again.
        if (cancel.cancelled() || !waitForPendingRoom(cancel)) {
            res->cancelled = true;
            return res;
        }
        // Premultiplied before resampling/blurring (averaging straight-alpha pixels bleeds the
        // color of transparent texels into edges: dark halos around logos) and already reduced to
        // the display size, so the blur below only touches output pixels.
        log::step("image", "decode {} ({} bytes)", traceName(res->key), bytes.size());
        auto decoded = decodeFitted(bytes, spec.width, spec.height, spec.fit);
        if (!decoded || decoded->empty()) {
            res->error = "decode failed";
            return res;
        }
        log::step("image", "decoded {} -> {}x{}", traceName(res->key), decoded->width, decoded->height);
        Pixmap pix = std::move(*decoded);
        if (spec.blur > 0) {
            // A blurred image has no detail to keep: blur a copy shrunk by up to sigma (at most
            // 4x, leaving >= 1 px of blur to hide the bilinear upscale when it is drawn).
            const int factor = std::clamp(static_cast<int>(spec.blur), 1, 4);
            if (factor > 1)
                pix = resize(pix, std::max(1, (pix.width + factor / 2) / factor),
                             std::max(1, (pix.height + factor / 2) / factor));
            blur(pix, spec.blur / static_cast<float>(factor));
        }
        res->dominant = dominantColor(pix);
        res->pixmap = std::move(pix);
        res->charged = res->pixmap.bytes();
        chargePending(res->charged);
        res->ok = true;
        return res;
    }

    static std::shared_ptr<Result> failure(std::string key, uint64_t gen, std::string error, bool transient = false) {
        auto res = std::make_shared<Result>();
        res->key = std::move(key);
        res->generation = gen;
        res->error = std::move(error);
        res->transient = transient;
        return res;
    }

    static std::shared_ptr<Result> cancelled(std::string key, uint64_t gen) {
        auto res = std::make_shared<Result>();
        res->key = std::move(key);
        res->generation = gen;
        res->cancelled = true;
        return res;
    }

    // ImageCache is a never-destroyed singleton; finish() ignores results after shutdown().
    static void post(std::shared_ptr<Result> res) {
        main_thread::post([res = std::move(res)]() mutable { ImageCache::instance().finish(std::move(res)); });
    }

    static void fetchHttp(std::string key, uint64_t gen, const ImageRequest& req, DecodeSpec spec, std::string path,
                          CancelToken cancel) {
        http::Request r;
        r.url = req.url;
        r.headers = req.headers;
        r.timeoutMs = 30000;
        r.maxBodyBytes = 24u * 1024 * 1024;
        r.cancel = cancel;
        r.background = true;
        const bool cacheToDisk = req.diskCache;
        http::Client::instance().send(
            std::move(r),
            [key = std::move(key), gen, spec, path = std::move(path), cacheToDisk,
             cancel = std::move(cancel)](http::Response&& resp) {
                if (resp.cancelled()) {
                    post(ImageCacheAccess::cancelled(key, gen));
                    return;
                }
                if (!resp.ok()) {
                    const bool transient = resp.status == 0 || resp.status >= 500 || resp.status == 408 || resp.status == 429;
                    post(failure(key, gen, resp.describe(), transient));
                    return;
                }
                auto body = std::make_shared<std::string>(std::move(resp.body));
                workers().post([key, gen, spec, path, cacheToDisk, body, cancel] {
                    auto res = decode(key, gen, *body, spec, cancel);
                    if (res->ok && cacheToDisk) writeDiskEntry(path, *body);
                    post(std::move(res));
                });
            },
            http::CallbackThread::Http);
    }
};

// ---------------------------------------------------------------------------------------------

const Texture* ImageRef::texture() {
    ImageCache::instance().touch(*this);
    return texture_.get();
}

TexturePtr ImageRef::textureShared() {
    ImageCache::instance().touch(*this);
    return texture_;
}

float ImageRef::revealProgress(double t, float duration) const {
    if (state_ != ImageState::Ready) return 0.0f;
    if (duration <= 0) return 1.0f;
    return static_cast<float>(std::clamp((t - readyTime_) / duration, 0.0, 1.0));
}

void ImageRef::onSettled(std::function<void(ImageRef&)> callback) {
    if (!callback) return;
    if (state_ == ImageState::Ready || state_ == ImageState::Failed) {
        callback(*this);
        return;
    }
    settled_.push_back(std::move(callback));
}

// ---------------------------------------------------------------------------------------------

ImageCache::ImageCache() = default;
ImageCache::~ImageCache() = default;

ImageCache& ImageCache::instance() {
    static ImageCache cache;
    return cache;
}

void ImageCache::init() {
    if (initialized_) return;
    const bool applet = platform::isAppletMode();
    const int defMb = applet ? 64 : 192;
    budget_ = static_cast<size_t>(std::max(16, Settings::instance().get<int>("ui.image_cache_mb", defMb))) << 20;
    diskBudget_ = static_cast<size_t>(std::max(0, Settings::instance().get<int>("ui.image_disk_cache_mb", 256))) << 20;
    if (applet) maxActive_ = 4;
    initialized_ = true;
    setPendingStopped(false);
    const std::string root = imageDir();
    g_diskBudget.store(diskBudget_);
    workers().post([root] {
        const char* hex = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) fs::ensureDir(root + "/" + hex[i]);
        loadDiskState();
    });
    log::info("image", "cache ready: {} MiB textures, {} MiB disk", budget_ >> 20, diskBudget_ >> 20);
}

void ImageCache::shutdown() {
    if (initialized_) saveDiskState();
    initialized_ = false;
    setPendingStopped(true);  // workers waiting for upload room give up (pool shutdown joins them)
    queue_.clear();
    uploads_.clear();
    lru_.clear();
    for (auto& [key, ref] : entries_) {
        ref->cancel_.cancel();
        ref->texture_.reset();
        ref->inLru_ = false;
        ref->state_ = ImageState::Idle;
        ref->settled_.clear();
    }
    entries_.clear();
    used_ = 0;
    active_ = 0;
    boosting_ = false;
    g_boostWanted.store(false);
    syncBoost();
}

std::string ImageCache::memoryKey(const ImageRequest& r) {
    const std::string& id = r.cacheKey.empty() ? r.url : r.cacheKey;
    return std::format("{}|{}x{}|{}|{:.1f}", id, r.width, r.height, static_cast<int>(r.fit), r.blur);
}

ImagePtr ImageCache::find(const std::string& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second;
}

ImagePtr ImageCache::load(const ImageRequest& request) {
    std::string key = memoryKey(request);
    if (auto it = entries_.find(key); it != entries_.end()) {
        touch(*it->second);
        return it->second;
    }
    auto ref = std::make_shared<ImageRef>();
    ref->key_ = key;
    ref->diskKey_ = std::format("{:016x}", fnv1a(request.cacheKey.empty() ? request.url : request.cacheKey));
    ref->request_ = request;
    ref->lastUsedFrame_ = frame_;
    entries_.emplace(std::move(key), ref);
    if (request.url.empty()) {
        ref->state_ = ImageState::Failed;
        return ref;
    }
    enqueue(*ref);
    return ref;
}

ImagePtr ImageCache::fromPixmap(const std::string& key, Pixmap pixmap) {
    if (auto it = entries_.find(key); it != entries_.end()) return it->second;
    auto ref = std::make_shared<ImageRef>();
    ref->key_ = key;
    ref->pinned_ = true;
    ref->lastUsedFrame_ = frame_;
    if (!pixmap.empty()) {
        ref->dominant_ = dominantColor(pixmap);
        ref->hasDominant_ = true;
        premultiply(pixmap);
        ref->texture_ = Texture::create(pixmap.width, pixmap.height, TextureFormat::RGBA8, pixmap.rgba.data());
        ref->width_ = pixmap.width;
        ref->height_ = pixmap.height;
        ref->state_ = ImageState::Ready;
        used_ += ref->texture_->bytes();
    } else {
        ref->state_ = ImageState::Failed;
    }
    entries_.emplace(key, ref);
    return ref;
}

void ImageCache::touch(ImageRef& ref) {
    ref.lastUsedFrame_ = frame_;
    if (ref.inLru_) {
        if (ref.lruIt_ != lru_.begin()) lru_.splice(lru_.begin(), lru_, ref.lruIt_);
    } else if (ref.state_ == ImageState::Idle && !ref.queued_ && !ref.request_.url.empty()) {
        enqueue(ref);
    } else if (ref.state_ == ImageState::Failed && ref.transientFailure_ && ref.failures_ < kMaxRetries &&
               !ref.queued_ && now() - ref.failTime_ > kRetryDelay * ref.failures_) {
        // Drawn again after a network error: try again (backoff grows per failure).
        ref.state_ = ImageState::Idle;
        enqueue(ref);
    }
}

void ImageCache::enqueue(ImageRef& ref) {
    if (!initialized_) return;
    auto it = entries_.find(ref.key_);
    if (it == entries_.end()) return;
    ref.queued_ = true;
    queue_.push_back(it->second);
}

void ImageCache::start(ImageRef& ref) {
    ref.state_ = ImageState::Loading;
    const uint64_t gen = ++ref.generation_;
    ++active_;
    const ImageRequest& req = ref.request_;
    DecodeSpec spec{req.width, req.height, req.fit, req.blur};
    std::string key = ref.key_;
    ref.cancel_.reset();
    CancelToken cancel = ref.cancel_;
    log::step("image", "load {}", traceName(key));

    if (isLocal(req.url)) {
        workers().post([key, gen, spec, url = req.url, cancel] {
            auto bytes = fs::readFile(url);
            if (!bytes) {
                ImageCacheAccess::post(ImageCacheAccess::failure(key, gen, "file not found: " + url));
                return;
            }
            ImageCacheAccess::post(ImageCacheAccess::decode(key, gen, *bytes, spec, cancel));
        });
        return;
    }

    std::string path = diskPath(ref.diskKey_);
    if (!req.diskCache) {
        ref.fetching_ = true;
        ImageCacheAccess::fetchHttp(std::move(key), gen, req, spec, std::move(path), std::move(cancel));
        return;
    }
    workers().post([key, gen, spec, path, req, cancel] {
        if (auto bytes = fs::readFile(path); bytes && !bytes->empty()) {
            if (const auto payload = diskPayload(*bytes)) {
                auto res = ImageCacheAccess::decode(key, gen, *payload, spec, cancel);
                if (res->ok || res->cancelled) {
                    ImageCacheAccess::post(std::move(res));
                    return;
                }
            }
            fs::removeFile(path);  // truncated, corrupt or old-format entry: refetch
        }
        main_thread::post([key, gen, spec, path, req, cancel] {
            ImageCache& cache = ImageCache::instance();
            if (!cache.initialized_) return;
            auto it = cache.entries_.find(key);
            if (it == cache.entries_.end() || it->second->generation_ != gen ||
                it->second->state_ != ImageState::Loading) {
                if (cache.active_ > 0) --cache.active_;
                return;
            }
            it->second->fetching_ = true;
            ImageCacheAccess::fetchHttp(key, gen, req, spec, path, cancel);
        });
    });
}

void ImageCache::finish(std::shared_ptr<Result> res) {
    if (!initialized_) return;
    if (active_ > 0) --active_;
    auto it = entries_.find(res->key);
    if (it == entries_.end()) return;
    ImageRef& ref = *it->second;
    if (ref.generation_ != res->generation || ref.state_ != ImageState::Loading) return;
    ref.fetching_ = false;
    if (res->cancelled) {
        ref.state_ = ImageState::Idle;  // reloads transparently if it is drawn again
        return;
    }
    if (!res->ok) {
        fail(ref, *res);
        return;
    }
    uploads_.push_back(std::move(res));
}

void ImageCache::fail(ImageRef& ref, const Result& res) {
    ref.state_ = ImageState::Failed;
    ref.failTime_ = now();
    ++ref.failures_;
    ref.transientFailure_ = res.transient;
    log::debug("image", "load failed {}: {}", ref.request_.url, res.error);
    log::step("image", "failed {}: {}", traceName(ref.key_), res.error);
    settle(ref);
}

void ImageCache::settle(ImageRef& ref) {
    auto callbacks = std::move(ref.settled_);
    ref.settled_.clear();
    // Callbacks may load more images (rehashing entries_); keep `ref` alive regardless.
    auto it = entries_.find(ref.key_);
    const ImagePtr keepAlive = it != entries_.end() ? it->second : nullptr;
    for (auto& cb : callbacks) cb(ref);
}

ImageRef* ImageCache::uploadTarget(const Result& res) {
    auto it = entries_.find(res.key);
    if (it == entries_.end()) return nullptr;
    ImageRef& ref = *it->second;
    if (ref.generation_ != res.generation || ref.state_ != ImageState::Loading) return nullptr;
    return &ref;
}

void ImageCache::finishUpload(ImageRef& ref, Result& res) {
    const Pixmap& pix = res.pixmap;
    ref.texture_ = std::move(res.texture);
    ref.width_ = pix.width;
    ref.height_ = pix.height;
    ref.dominant_ = res.dominant;
    ref.hasDominant_ = true;
    ref.state_ = ImageState::Ready;
    ref.failures_ = 0;
    ref.readyTime_ = now();
    used_ += ref.texture_->bytes();
    lru_.push_front(&ref);
    ref.lruIt_ = lru_.begin();
    ref.inLru_ = true;
    log::step("image", "ready {}", traceName(ref.key_));
    settle(ref);
}

void ImageCache::cancelUnwanted() {
    for (auto& [key, sp] : entries_) {
        ImageRef& r = *sp;
        if (r.state_ != ImageState::Loading) continue;
        // Only the cache holds it (e.g. a recycled grid cell asked for another image) and it has
        // not been drawn for a while: free the transfer slot for visible images. Also reaches
        // loads still reading the disk or waiting for upload room (decode() checks the token).
        if (sp.use_count() == 1 && frame_ - r.lastUsedFrame_ > kCancelAfterFrames) {
            r.cancel_.cancel();
            r.fetching_ = false;
        }
    }
}

void ImageCache::evict(ImageRef& ref) {
    if (!ref.inLru_ || ref.pinned_) return;
    if (ref.texture_) used_ -= std::min(used_, ref.texture_->bytes());
    ref.texture_.reset();
    lru_.erase(ref.lruIt_);
    ref.inLru_ = false;
    ref.state_ = ImageState::Idle;
}

void ImageCache::update() {
    if (!initialized_) return;
    ++frame_;

    // Texture uploads: decoded results land in batches. A single glTexImage2D of a 1080p
    // backdrop (8 MiB) took a whole frame, so textures are allocated empty and filled in bands
    // of rows, bounded per frame by bytes, completed images and measured time. At least one band
    // goes up every frame; the image becomes Ready once its last band is in.
    {
        const auto start = perf::Clock::now();
        size_t bytes = 0;
        int completed = 0;
        while (!uploads_.empty()) {
            Result& res = *uploads_.front();
            ImageRef* ref = uploadTarget(res);
            const Pixmap& pix = res.pixmap;
            if (!ref || pix.empty()) {
                uploads_.pop_front();  // reloaded or dropped meanwhile
                continue;
            }
            if (res.rowsUploaded == 0 && entries_.find(res.key)->second.use_count() == 1 &&
                frame_ - ref->lastUsedFrame_ > kCancelAfterFrames) {
                // Nobody wants it any more: free the pixels now instead of uploading them; the
                // image reloads (from the disk cache) if it is drawn again.
                ref->state_ = ImageState::Idle;
                uploads_.pop_front();
                continue;
            }
            const size_t rowBytes = static_cast<size_t>(pix.width) * 4;
            if (!res.texture) {
                log::step("image", "upload {} {}x{}", traceName(res.key), pix.width, pix.height);
                res.texture = Texture::create(pix.width, pix.height, TextureFormat::RGBA8, nullptr);
            }
            const size_t room = uploadBytesPerFrame_ > bytes ? uploadBytesPerFrame_ - bytes : 0;
            const size_t bandRows = std::max<size_t>(1, std::min(room, kUploadBandBytes) / rowBytes);
            const int rows = static_cast<int>(std::min<size_t>(bandRows, static_cast<size_t>(pix.height - res.rowsUploaded)));
            res.texture->update(0, res.rowsUploaded, pix.width, rows,
                                pix.rgba.data() + static_cast<size_t>(res.rowsUploaded) * rowBytes);
            res.rowsUploaded += rows;
            bytes += static_cast<size_t>(rows) * rowBytes;
            if (res.rowsUploaded >= pix.height) {
                std::shared_ptr<Result> done = std::move(uploads_.front());
                uploads_.pop_front();
                finishUpload(*ref, *done);
                ++completed;
            }
            if (bytes >= uploadBytesPerFrame_ || completed >= uploadsPerFrame_ ||
                perf::msSince(start) >= uploadMsPerFrame_)
                break;
        }
        if (bytes > 0) {
            perf::Frame& pf = perf::frame();
            pf.uploads += completed;
            pf.uploadBytes += bytes;
            pf.uploadMs += perf::msSince(start);
        }
    }

    // Start queued loads, most recently used first (visible items win while scrolling).
    while (active_ < maxActive_ && !queue_.empty()) {
        size_t best = SIZE_MAX;
        uint64_t bestFrame = 0;
        for (size_t i = 0; i < queue_.size(); ++i) {
            auto sp = queue_[i].lock();
            if (!sp) continue;
            if (best == SIZE_MAX || sp->lastUsedFrame_ >= bestFrame) {
                best = i;
                bestFrame = sp->lastUsedFrame_;
            }
        }
        if (best == SIZE_MAX) {
            queue_.clear();
            break;
        }
        auto sp = queue_[best].lock();
        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(best));
        sp->queued_ = false;
        if (sp->state_ != ImageState::Idle) continue;
        // Nobody holds it and it was not drawn lately: skip (reloads if touched again).
        if (sp.use_count() <= 2 && frame_ - sp->lastUsedFrame_ > 3) continue;
        start(*sp);
    }

    if (frame_ % 10 == 0) cancelUnwanted();

    while (used_ > budget_ && !lru_.empty()) {
        ImageRef* victim = lru_.back();
        if (victim->lastUsedFrame_ + 1 >= frame_) break;  // everything left is on screen
        evict(*victim);
    }

    if (frame_ % 120 == 0) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            const ImageRef& r = *it->second;
            const bool idle = r.state_ == ImageState::Idle || r.state_ == ImageState::Failed;
            if (idle && !r.queued_ && it->second.use_count() == 1) it = entries_.erase(it);
            else ++it;
        }
    }

    if (active_ > 0 || !uploads_.empty()) {
        idleFrames_ = 0;
        if (!boosting_) {
            boosting_ = true;
            g_boostWanted.store(true);
            workers().post(&syncBoost);
        }
    } else if (boosting_ && ++idleFrames_ > kBoostLingerFrames) {
        boosting_ = false;
        g_boostWanted.store(false);
        workers().post(&syncBoost);
    }
}

void ImageCache::trimMemory() {
    for (auto it = lru_.begin(); it != lru_.end();) {
        ImageRef* r = *it++;
        if (r->lastUsedFrame_ + 1 < frame_) evict(*r);
    }
}

void ImageCache::clearDiskCache() {
    const std::string root = imageDir();
    workers().post([root] {
        fs::removeTree(root);
        const char* hex = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) fs::ensureDir(root + "/" + hex[i]);
        g_diskBytes.store(0);
        g_diskScanTime.store(unixNow());
        saveDiskState();
        log::info("image", "disk cache cleared");
    });
}

}  // namespace karafin::gfx
