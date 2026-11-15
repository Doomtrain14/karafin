// One download job: probe -> bundle metadata/images/subtitles -> ranged transfer into <file>.part
// -> rename. Runs on a dl-N thread; all blocking network calls use Call<T>::wait / perform.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>

#include <unistd.h>

#include "core/fs.hpp"
#include "core/http.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "downloads/detail/manager_impl.hpp"
#include "downloads/detail/part_writer.hpp"
#include "downloads/detail/sdcard.hpp"
#include "downloads/detail/util.hpp"
#include "jellyfin/accounts.hpp"
#include "jellyfin/error.hpp"

namespace karafin::downloads {

using detail::Job;
using detail::Queue;
using detail::StopReason;
using detail::Task;

namespace {

constexpr const char* kTag = "downloads";
constexpr uint64_t kSyncEveryBytes = 64ull * 1024 * 1024;
constexpr double kSyncEverySeconds = 20.0;
constexpr double kProgressInterval = 0.25;  // ~4 updates per second
constexpr int kMaxRestarts = 3;

using Clock = std::chrono::steady_clock;

constexpr double kOfflinePollSeconds = 1.0;

enum class Outcome { Done, Retry, Restart, Fatal, Stopped };

struct StepResult {
    Outcome kind = Outcome::Done;
    std::string message;
    bool progressed = false;
    // Retry only: the server could not be reached. Waits (with backoff) for as long as it takes
    // instead of counting towards kMaxAttempts.
    bool connectivity = false;

    static StepResult done() { return {}; }
    static StepResult stopped() { return {Outcome::Stopped, {}, false, false}; }
    static StepResult restart() { return {Outcome::Restart, {}, false, false}; }
    static StepResult fatal(std::string m) { return {Outcome::Fatal, std::move(m), false, false}; }
    static StepResult retry(std::string m, bool progressed = false) {
        return {Outcome::Retry, std::move(m), progressed, false};
    }
    static StepResult unreachable(std::string m, bool progressed = false) {
        return {Outcome::Retry, std::move(m), progressed, true};
    }
};

std::string fat32Message(uint64_t size) {
    return std::format("This file is {} - larger than the 4 GB limit of FAT32 SD cards. Reformat the SD card as "
                       "exFAT to download it.",
                       detail::formatBytes(size));
}

// `end`: file size the failed write would have reached.
std::string writeErrorMessage(int err, uint64_t end) {
    if (err == ENOSPC) return "The SD card is full. Free up some space, then retry.";
    if (err == EFBIG || end > detail::kFat32MaxFileSize) return fat32Message(end);
    return std::format("Couldn't write to the SD card ({}).", err ? std::strerror(err) : "unknown error");
}

std::string unreachableMessage(const http::Response& resp) {
    return std::format("Can't reach the server ({}).", http::errorName(resp.error));
}

// Transport failures that mean "server not reachable right now" (vs. TLS / protocol problems).
bool isConnectivityError(http::Error e) {
    return e == http::Error::Resolve || e == http::Error::Connect || e == http::Error::Timeout ||
           e == http::Error::Network;
}

std::string sessionExpiredMessage() {
    return "Your session on this server has expired. Sign in again, then retry the download.";
}

std::string permissionMessage() {
    http::Response forbidden;
    forbidden.status = 403;
    return jellyfin::ApiError::fromResponse(forbidden, jellyfin::ErrorContext::Download).message;
}

class JobRunner {
public:
    JobRunner(DownloadManager::Impl& m, std::shared_ptr<Job> job) : m_(m), job_(std::move(job)) {}

    void run();

private:
    bool stopRequested() const { return job_->reason.load() != StopReason::None; }
    // Applies `fn` to the live task under the lock (false if it no longer exists).
    template <class F>
    bool withTask(F&& fn, bool emitEvent, bool urgent);
    bool setState(State s, std::string error = {});
    void reportProgress(uint64_t bytes, uint64_t total, double speed, double eta);
    // bytesDone + syncedBytes after the .part changed outside the streaming path.
    void setPart(uint64_t bytes, uint64_t synced);
    void setSynced(uint64_t synced, bool urgent);
    uint64_t reservedByOthers() const;

    StepResult attempt();
    StepResult bundleAssets(const jellyfin::ClientPtr& client);
    StepResult transfer(const jellyfin::ClientPtr& client, const std::string& partPath, uint64_t partSize,
                        uint64_t total);
    StepResult finalize(const std::string& partPath, uint64_t size);
    StepResult fromApiError(const jellyfin::ApiError& e);

    // Missing: the server had nothing usable (optional asset, skipped). WriteFailed: the SD card
    // write failed (storage error, fails the job).
    enum class Fetch { Ok, Missing, Unreachable, WriteFailed, Stopped };
    Fetch fetchToFile(const std::string& url, const std::string& path, std::string& error);

    void fail(const std::string& message);
    // Sleeps up to `seconds`; wakes early on pause / shutdown and, when `untilOnline`, as soon as
    // the console has a network connection again.
    void waitBackoff(double seconds, bool untilOnline);
    void handleStop();

    DownloadManager::Impl& m_;
    std::shared_ptr<Job> job_;
    Task task_;  // working copy, refreshed at the start of every attempt
    std::string dir_;
    std::string serverPath_;
};

template <class F>
bool JobRunner::withTask(F&& fn, bool emitEvent, bool urgent) {
    std::lock_guard lock(m_.mutex);
    Task* t = m_.queue.find(job_->key);
    if (!t) return false;
    fn(*t);
    if (emitEvent) m_.emit(EventType::Updated, *t);
    m_.markDirty(urgent);
    return true;
}

bool JobRunner::setState(State s, std::string error) {
    std::lock_guard lock(m_.mutex);
    if (stopRequested()) return false;
    Task* t = m_.queue.find(job_->key);
    if (!t || !Queue::transition(*t, s, std::move(error))) return false;
    task_.info.state = s;
    m_.emit(EventType::Updated, *t);
    m_.markDirty(true);
    m_.updateAwake();
    return true;
}

void JobRunner::reportProgress(uint64_t bytes, uint64_t total, double speed, double eta) {
    std::lock_guard lock(m_.mutex);
    Task* t = m_.queue.find(job_->key);
    if (!t) return;
    DownloadInfo& i = t->info;
    i.bytesDone = bytes;
    if (total > 0) i.bytesTotal = total;
    if (i.state == State::Downloading) {
        i.bytesPerSecond = speed;
        i.etaSeconds = eta;
    }
    const auto now = Clock::now();
    if (now - t->lastEmit >= std::chrono::milliseconds(240)) {
        t->lastEmit = now;
        m_.emit(EventType::Progress, *t);
    }
    m_.markDirty(false);
}

void JobRunner::setPart(uint64_t bytes, uint64_t synced) {
    task_.syncedBytes = synced;
    withTask(
        [bytes, synced](Task& t) {
            t.info.bytesDone = bytes;
            t.syncedBytes = synced;
        },
        true, true);
}

void JobRunner::setSynced(uint64_t synced, bool urgent) {
    task_.syncedBytes = synced;
    withTask([synced](Task& t) { t.syncedBytes = synced; }, false, urgent);
}

uint64_t JobRunner::reservedByOthers() const {
    std::lock_guard lock(m_.mutex);
    uint64_t sum = 0;
    for (const auto& t : m_.queue.tasks())
        if (t.running && t.info.key != job_->key && t.info.bytesTotal > t.info.bytesDone)
            sum += t.info.bytesTotal - t.info.bytesDone;
    return sum;
}

void JobRunner::run() {
    int attempts = 0;      // consecutive counted failures -> Failed at kMaxAttempts
    int unreachable = 0;   // consecutive connectivity failures (backoff only)
    int restarts = 0;
    for (;;) {
        if (stopRequested()) return handleStop();
        StepResult r = attempt();
        if (r.kind == Outcome::Done) return;
        if (stopRequested() || r.kind == Outcome::Stopped) return handleStop();

        switch (r.kind) {
            case Outcome::Fatal:
                log::warn(kTag, "{} failed: {}", job_->key, r.message);
                return fail(r.message);
            case Outcome::Restart:
                if (++restarts > kMaxRestarts) return fail("The file keeps changing on the server. Retry later.");
                log::info(kTag, "{}: restarting from the beginning", job_->key);
                continue;
            case Outcome::Retry: {
                if (r.progressed) attempts = unreachable = 0;
                const bool online = platform::networkConnected();
                double wait = kOfflinePollSeconds;
                std::string msg = "Waiting for a network connection.";
                if (online && r.connectivity) {
                    ++unreachable;
                    wait = detail::backoffSeconds(unreachable);
                    msg = r.message;
                } else if (online) {
                    if (++attempts >= detail::kMaxAttempts) {
                        log::warn(kTag, "{} giving up after {} attempts: {}", job_->key, attempts, r.message);
                        return fail(r.message);
                    }
                    wait = detail::backoffSeconds(attempts);
                    msg = r.message;
                }
                const int shownAttempt = std::max(attempts, unreachable);
                log::info(kTag, "{}: {} - retry {} in {:.0f}s", job_->key, msg, shownAttempt, wait);
                bool waiting = false;
                {
                    std::lock_guard lock(m_.mutex);
                    Task* t = stopRequested() ? nullptr : m_.queue.find(job_->key);
                    if (t && Queue::transition(*t, State::WaitingForNetwork, msg)) {
                        waiting = true;
                        t->info.attempt = shownAttempt;
                        m_.retryAt[job_->key] =
                            Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(wait * 1000.0));
                        m_.emit(EventType::Updated, *t);
                        m_.markDirty(true);
                        m_.updateAwake();
                    }
                }
                if (!waiting) return handleStop();
                // Offline: poll until the connection is back (wake from sleep, Wi-Fi returns),
                // then retry right away.
                waitBackoff(online ? wait : 24.0 * 3600.0, !online);
                {
                    std::lock_guard lock(m_.mutex);
                    m_.retryAt.erase(job_->key);
                }
                continue;
            }
            case Outcome::Done:
            case Outcome::Stopped: break;
        }
    }
}

void JobRunner::fail(const std::string& message) {
    // A pause / cancel that raced with the error wins (and still gets its cleanup).
    if (!setState(State::Failed, message) && stopRequested()) handleStop();
}

void JobRunner::waitBackoff(double seconds, bool untilOnline) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));
    std::unique_lock lock(m_.mutex);
    for (;;) {
        if (stopRequested() || m_.stopping) return;
        const auto now = Clock::now();
        if (now >= deadline) return;
        auto slice = deadline - now;
        if (untilOnline) slice = std::min<Clock::duration>(slice, std::chrono::milliseconds(1000));
        m_.cv.wait_for(lock, slice, [this] { return stopRequested() || m_.stopping; });
        if (untilOnline) {
            lock.unlock();
            const bool online = platform::networkConnected();
            lock.lock();
            if (online) return;
        }
    }
}

void JobRunner::handleStop() {
    const StopReason reason = job_->reason.load();
    switch (reason) {
        case StopReason::Remove: {
            if (job_->serverId.empty() || job_->itemId.empty()) break;
            const std::string dir = detail::itemDir(job_->serverId, job_->itemId);
            if (!fs::removeTree(dir)) log::warn(kTag, "could not delete {}", dir);
            rmdir(detail::serverDir(job_->serverId).c_str());
            log::info(kTag, "{} cancelled", job_->key);
            break;
        }
        case StopReason::Shutdown: {
            std::lock_guard lock(m_.mutex);
            if (Task* t = m_.queue.find(job_->key); t && isActive(t->info.state)) {
                Queue::transition(*t, State::Paused);
                t->info.pausedByUser = false;
                m_.markDirty(true);
            }
            break;
        }
        case StopReason::Pause: log::info(kTag, "{} paused", job_->key); break;
        case StopReason::None: break;
    }
}

StepResult JobRunner::fromApiError(const jellyfin::ApiError& e) {
    using jellyfin::ErrorKind;
    switch (e.kind) {
        case ErrorKind::Cancelled: return StepResult::stopped();
        case ErrorKind::Network:
        case ErrorKind::Offline:
            return StepResult::unreachable(e.message.empty() ? std::string("Can't reach the server.") : e.message);
        case ErrorKind::Server:
            if (e.status == 502 || e.status == 503 || e.status == 504) return StepResult::unreachable(e.message);
            return StepResult::retry(e.message.empty() ? std::string("The server hit an error.") : e.message);
        case ErrorKind::Unauthorized:
            m_.forgetClient(task_.info.serverId, task_.info.userId);
            return StepResult::fatal(sessionExpiredMessage());
        case ErrorKind::Forbidden: return StepResult::fatal(permissionMessage());
        case ErrorKind::NotFound: return StepResult::fatal("This item is no longer available on the server.");
        default: return StepResult::fatal(e.message.empty() ? std::string("The download failed.") : e.message);
    }
}

StepResult JobRunner::attempt() {
    {
        std::lock_guard lock(m_.mutex);
        const Task* t = m_.queue.find(job_->key);
        if (!t) return StepResult::stopped();
        task_ = *t;
    }
    if (!setState(State::Probing)) return StepResult::stopped();
    const DownloadInfo& info = task_.info;
    dir_ = detail::itemDir(info.serverId, info.itemId);
    if (!fs::ensureDir(dir_)) return StepResult::fatal("Couldn't create the download folder on the SD card.");

    auto client = m_.clientFor(info.serverId, info.userId);
    if (!client) {
        std::string name = "the server";
        if (auto s = jellyfin::AccountStore::instance().server(info.serverId); s && !s->name.empty()) name = s->name;
        return StepResult::fatal(std::format("Sign in to {} again to download this item.", name));
    }

    if (!task_.assetsDone) {
        StepResult r = bundleAssets(client);
        if (r.kind != Outcome::Done) return r;
    }

    auto probe = client->probeDownload(info.itemId).wait(job_->token);
    if (!probe) return fromApiError(probe.error);

    const uint64_t total = probe->contentLength > 0 ? probe->contentLength : task_.info.bytesTotal;
    if (task_.info.fileName.empty())
        task_.info.fileName =
            "media." + detail::mediaExtension(task_.info.container, probe->fileName, serverPath_);
    const std::string finalPath = dir_ + "/" + task_.info.fileName;
    const std::string partPath = finalPath + detail::kPartSuffix;
    const uint64_t fileSize = fs::fileSize(partPath).value_or(0);
    uint64_t partSize = detail::resumeOffset(fileSize, task_.syncedBytes);
    if (partSize < fileSize)
        log::info(kTag, "{}: .part is {} but only {} were synced; resuming from there", job_->key, fileSize, partSize);

    const bool etagChanged = !task_.eTag.empty() && !probe->eTag.empty() && task_.eTag != probe->eTag;
    const bool modifiedChanged = task_.eTag.empty() && !task_.lastModified.empty() && !probe->lastModified.empty() &&
                                 task_.lastModified != probe->lastModified;
    const bool sizeChanged =
        task_.info.bytesTotal > 0 && probe->contentLength > 0 && task_.info.bytesTotal != probe->contentLength;
    const bool changed = etagChanged || modifiedChanged || sizeChanged;
    if (partSize > 0) {
        if (changed) {
            log::info(kTag, "{}: file changed on the server; restarting", job_->key);
            fs::removeFile(partPath);
            partSize = 0;
        } else if (!probe->acceptRanges) {
            log::info(kTag, "{}: server does not support resuming; restarting", job_->key);
            partSize = 0;
        } else if (total > 0 && partSize > total) {
            partSize = 0;
        }
    }

    task_.eTag = probe->eTag;
    task_.lastModified = probe->lastModified;
    task_.info.bytesTotal = total;
    withTask(
        [&](Task& t) {
            t.eTag = task_.eTag;
            t.lastModified = task_.lastModified;
            t.info.fileName = task_.info.fileName;
            t.info.bytesTotal = total;
            t.info.bytesDone = partSize;
        },
        true, true);

    // An earlier finalize() moved the file into place but could not record it (manifest write
    // failed, crash): finish without downloading it again.
    if (total > 0 && !changed && !fs::exists(partPath) && fs::fileSize(finalPath) == total) {
        log::info(kTag, "{}: {} is already complete on the SD card", job_->key, task_.info.fileName);
        return finalize(partPath, total);
    }

    if (total > 0) {
        // statvfs reporting a zero-sized card means "unknown", not "full".
        if (auto sp = fs::space(paths::downloadsDir()); sp && sp->totalBytes > 0) {
            // The other running transfer will take its remaining bytes too; .part bytes past the
            // resume offset are about to be cut off.
            const uint64_t onDisk = fs::fileSize(partPath).value_or(0);
            const uint64_t need = total - partSize + detail::kSpaceMargin + reservedByOthers();
            const uint64_t avail = sp->freeBytes + (onDisk > partSize ? onDisk - partSize : 0);
            if (avail < need) {
                return StepResult::fatal(std::format("Not enough free space on the SD card: {} needed, {} free.",
                                                     detail::formatBytes(need), detail::formatBytes(avail)));
            }
        }
    }

    if (total > 0 && partSize == total) return finalize(partPath, total);
    return transfer(client, partPath, partSize, total);
}

JobRunner::Fetch JobRunner::fetchToFile(const std::string& url, const std::string& path, std::string& error) {
    http::Request req;
    req.url = url;
    req.cancel = job_->token;
    req.timeoutMs = 60000;
    http::Response resp = http::Client::instance().perform(std::move(req));
    if (resp.cancelled() || stopRequested()) return Fetch::Stopped;
    if (resp.ok()) {
        if (resp.body.empty()) return Fetch::Missing;
        if (!fs::writeFileAtomic(path, resp.body)) {
            const int err = errno;
            log::error(kTag, "{}: storage write failed: cannot write {} ({})", job_->key, path,
                       err ? std::strerror(err) : "unknown error");
            error = writeErrorMessage(err, 0);
            return Fetch::WriteFailed;
        }
        return Fetch::Ok;
    }
    if (isConnectivityError(resp.error)) {
        error = unreachableMessage(resp);
        return Fetch::Unreachable;
    }
    // Images and subtitles are optional: a server error just leaves this one out.
    log::info(kTag, "asset {} skipped: {}", url, resp.describe());
    return Fetch::Missing;
}

StepResult JobRunner::bundleAssets(const jellyfin::ClientPtr& client) {
    auto meta = client->offlineMetadata(task_.info.itemId).wait(job_->token);
    if (!meta) return fromApiError(meta.error);
    const jellyfin::BaseItem& item = meta->parsed;

    nlohmann::json doc = {
        {"version", 1},
        {"serverId", task_.info.serverId},
        {"userId", task_.info.userId},
        {"savedAt", detail::unixNow()},
        {"item", meta->item},
        {"series", meta->series},
        {"season", meta->season},
    };
    if (task_.info.positionTicks > 0 || task_.info.played)
        detail::patchUserData(doc, task_.info.positionTicks, task_.info.played);
    if (!fs::writeFileAtomic(dir_ + "/" + detail::kMetadataFile, detail::dumpJson(doc)))
        return StepResult::fatal(writeErrorMessage(errno, 0));
    m_.invalidateMeta(job_->key);

    const jellyfin::MediaSource* ms = item.mediaSource(task_.info.mediaSourceId);
    if (!ms) ms = item.mediaSource("");
    serverPath_ = ms && !ms->path.empty() ? ms->path : item.path;
    const std::string msId = ms ? ms->id : task_.info.mediaSourceId;

    withTask(
        [&](Task& t) {
            DownloadInfo& i = t.info;
            if (!item.name.empty()) i.name = item.name;
            if (!item.type.empty()) i.type = item.type;
            i.seriesId = item.seriesId;
            i.seriesName = item.seriesName;
            i.seasonId = item.seasonId;
            i.seasonName = item.seasonName;
            i.indexNumber = item.indexNumber;
            i.parentIndexNumber = item.parentIndexNumber;
            i.productionYear = item.productionYear;
            if (item.runTimeTicks > 0) i.runTimeTicks = item.runTimeTicks;
            if (!item.overview.empty()) i.overview = item.overview;
            i.mediaSourceId = msId;
            if (ms && !ms->container.empty()) i.container = ms->container;
            if (i.bytesTotal == 0 && ms && ms->size > 0) i.bytesTotal = static_cast<uint64_t>(ms->size);
            if (i.positionTicks == 0 && !i.played) {
                i.positionTicks = item.userData.playbackPositionTicks;
                i.played = item.userData.played;
            }
            task_.info = i;
        },
        true, true);

    // Images: server-side failures just leave the image out; connectivity and SD card write
    // failures don't.
    struct Image {
        std::string url;
        const char* file;
        bool ok = false;
    };
    Image images[3] = {
        {client->imageUrl(item, jellyfin::ImageType::Primary, {.maxWidth = 500}), detail::kPosterFile},
        {client->imageUrl(item, jellyfin::ImageType::Backdrop, {.maxWidth = 1280, .quality = 80}),
         detail::kBackdropFile},
        {item.isEpisode() && !item.seriesId.empty() && !item.seriesPrimaryImageTag.empty()
             ? client->itemImageUrl(item.seriesId, jellyfin::ImageType::Primary, item.seriesPrimaryImageTag,
                                    {.maxWidth = 500})
             : std::string(),
         detail::kSeriesPosterFile},
    };
    for (auto& img : images) {
        if (img.url.empty()) continue;
        std::string err;
        switch (fetchToFile(img.url, dir_ + "/" + img.file, err)) {
            case Fetch::Ok: img.ok = true; break;
            case Fetch::Missing: break;
            case Fetch::Unreachable: return StepResult::unreachable(err);
            case Fetch::WriteFailed: return StepResult::fatal(err);
            case Fetch::Stopped: return StepResult::stopped();
        }
    }

    std::vector<LocalSubtitle> subs;
    const auto external = client->externalSubtitleDownloads(item, msId);
    if (!external.empty()) fs::ensureDir(dir_ + "/" + detail::kSubtitleDir);
    for (const auto& s : external) {
        std::string err;
        switch (fetchToFile(s.url, dir_ + "/" + detail::kSubtitleDir + "/" + s.fileName, err)) {
            case Fetch::Ok: {
                LocalSubtitle l;
                l.index = s.index;
                l.fileName = s.fileName;
                l.language = s.language;
                l.title = s.title;
                l.format = s.format;
                l.isDefault = s.isDefault;
                l.isForced = s.isForced;
                l.isHearingImpaired = s.isHearingImpaired;
                subs.push_back(std::move(l));
                break;
            }
            case Fetch::Missing: log::warn(kTag, "{}: subtitle {} unavailable", job_->key, s.fileName); break;
            case Fetch::Unreachable: return StepResult::unreachable(err);
            case Fetch::WriteFailed: return StepResult::fatal(err);
            case Fetch::Stopped: return StepResult::stopped();
        }
    }

    withTask(
        [&](Task& t) {
            t.hasPoster = images[0].ok;
            t.hasBackdrop = images[1].ok;
            t.hasSeriesPoster = images[2].ok;
            t.subtitles = subs;
            t.assetsDone = true;
        },
        false, true);
    task_.assetsDone = true;
    log::info(kTag, "{}: bundled metadata, {} image(s), {} subtitle(s)", job_->key,
              int(images[0].ok) + int(images[1].ok) + int(images[2].ok), subs.size());
    return StepResult::done();
}

StepResult JobRunner::transfer(const jellyfin::ClientPtr& client, const std::string& partPath, uint64_t partSize,
                               uint64_t total) {
    // A plain file on a FAT32 card stops at 4 GiB - 1; a concatenation file doesn't (and is
    // harmless on exFAT). Unknown sizes get one too.
    if (partSize == 0 && (total == 0 || total > detail::kFat32MaxFileSize)) {
        if (detail::createLargeFile(partPath)) {
            log::info(kTag, "{}: writing into a concatenation file", job_->key);
        } else if (const auto exFat = detail::exFatSupported(); total > detail::kFat32MaxFileSize && exFat && !*exFat) {
            return StepResult::fatal(fat32Message(total));  // no exFAT driver: the card is FAT32
        }
    }

    detail::PartWriter w;
    if (!w.open(partPath, partSize)) return StepResult::fatal(writeErrorMessage(w.errorNumber(), partSize));
    // open() cut the file to the resume offset; record that before anything is appended.
    if (task_.syncedBytes != partSize) setSynced(partSize, true);
    if (!setState(State::Downloading)) {
        w.close(true);
        return StepResult::stopped();
    }
    reportProgress(partSize, total, 0, -1);
    log::info(kTag, "{}: downloading {} from {}{}", job_->key, task_.info.fileName, detail::formatBytes(partSize),
              total ? " of " + detail::formatBytes(total) : std::string());

    http::Request req = client->downloadRequest(task_.info.itemId);
    req.cancel = job_->token;
    if (partSize > 0) {
        req.rangeFrom = partSize;
        // A changed file then comes back whole (200) instead of as a mismatched range. Jellyfin's
        // file results carry Last-Modified but no ETag.
        if (detail::isStrongETag(task_.eTag)) {
            req.headers.emplace_back("If-Range", task_.eTag);
        } else if (!task_.lastModified.empty()) {
            req.headers.emplace_back("If-Range", task_.lastModified);
        }
    }

    const uint64_t expectedRemaining = total > partSize ? total - partSize : 0;
    bool decided = partSize == 0;
    bool abortRange = false;
    bool writeFailed = false;
    auto lastTick = Clock::now();
    auto lastSync = lastTick;
    uint64_t lastBytes = partSize;
    uint64_t lastSyncBytes = partSize;
    double speed = 0;

    req.onProgress = [&](const http::Progress& p) {
        if (decided || p.total == 0) return;
        decided = true;
        // A full-size body although we asked for a range: Range ignored or If-Range mismatch.
        if (total > 0 && p.total == total && p.total != expectedRemaining) {
            if (w.rebaseToZero()) {
                log::info(kTag, "{}: server sent the whole file; restarting in place", job_->key);
                lastBytes = lastSyncBytes = 0;
                setSynced(0, true);
            } else {
                abortRange = true;
            }
        }
    };
    req.onData = [&](std::string_view chunk) {
        if (abortRange) return false;
        if (m_.throttled.load(std::memory_order_relaxed)) m_.limiter.acquire(chunk.size(), job_->token);
        if (!w.write(chunk)) {
            writeFailed = true;
            return false;
        }
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - lastTick).count();
        if (dt >= kProgressInterval) {
            const uint64_t bytes = w.size();
            if (bytes >= lastBytes) {
                const double inst = static_cast<double>(bytes - lastBytes) / dt;
                speed = speed <= 0 ? inst : speed + (inst - speed) * (1.0 - std::exp(-dt / 3.0));
            }
            lastTick = now;
            lastBytes = bytes;
            const double eta = total > bytes && speed > 1 ? static_cast<double>(total - bytes) / speed : -1;
            reportProgress(bytes, total, speed, eta);
            if (bytes - std::min(bytes, lastSyncBytes) >= kSyncEveryBytes ||
                std::chrono::duration<double>(now - lastSync).count() >= kSyncEverySeconds) {
                if (!w.flush(true)) {
                    writeFailed = true;
                    return false;
                }
                setSynced(w.written(), false);
                lastSync = now;
                lastSyncBytes = bytes;
            }
        }
        return !job_->token.cancelled();
    };

    http::Response resp = http::Client::instance().perform(std::move(req));
    const long status = resp.status;

    bool bodyValid = false;
    if (status == 206) {
        // Only a range that starts exactly at the requested offset (and belongs to a file of the
        // expected size) may be appended; anything else is discarded and the file restarted.
        bodyValid = w.base() == partSize && partSize > 0;
        std::optional<detail::ContentRange> range;
        if (const auto* cr = resp.header("content-range")) range = detail::parseContentRange(*cr);
        if (!range) {
            if (bodyValid) log::warn(kTag, "{}: partial response without a valid Content-Range; restarting", job_->key);
            bodyValid = false;
        } else if (range->start != partSize || (range->total > 0 && total > 0 && range->total != total)) {
            if (bodyValid)
                log::warn(kTag, "{}: Content-Range {}-{}/{} doesn't match the request ({}/{}); restarting", job_->key,
                          range->start, range->end, range->total, partSize, total);
            bodyValid = false;
        }
        if (const auto* et = resp.header("etag"); et && !task_.eTag.empty() && *et != task_.eTag) bodyValid = false;
    } else if (status == 200) {
        // The whole file: it replaces the .part from byte 0 (or the .part is discarded below and
        // the download restarts when bytes were already appended at the old offset).
        bodyValid = w.base() == 0 || w.rebaseToZero();
        if (!bodyValid) log::info(kTag, "{}: server sent the whole file; restarting", job_->key);
    }
    if (!bodyValid) w.truncateToBase();
    const uint64_t gained = w.size() > partSize ? w.size() - partSize : 0;
    const bool progressed = gained >= detail::kProgressResetBytes;

    // Closes the .part (writing out the buffer when `keep`) and records what is on the card now.
    auto closePart = [&](bool keep) {
        const bool ok = w.close(keep);
        const uint64_t onDisk = fs::fileSize(partPath).value_or(0);
        const uint64_t synced = std::min(onDisk, ok ? w.written() : task_.syncedBytes);
        setPart(synced, synced);
        return ok;
    };
    auto discardPart = [&] {
        w.close(false);
        fs::removeFile(partPath);
        setPart(0, 0);
    };

    if (stopRequested() || resp.cancelled()) {
        closePart(bodyValid);
        return StepResult::stopped();
    }
    if (writeFailed) {
        const int err = w.errorNumber();
        const uint64_t end = w.size();
        closePart(false);
        return StepResult::fatal(writeErrorMessage(err, end));
    }
    if (abortRange || ((status == 200 || status == 206) && !bodyValid)) {
        discardPart();
        return StepResult::restart();
    }

    if (resp.error == http::Error::None && bodyValid) {
        const uint64_t end = w.size();
        if (!closePart(true)) return StepResult::fatal(writeErrorMessage(w.errorNumber(), end));
        const uint64_t size = fs::fileSize(partPath).value_or(0);
        uint64_t expect = total;
        if (expect == 0) {
            if (const auto* cr = resp.header("content-range")) {
                if (auto range = detail::parseContentRange(*cr)) expect = range->total;
            } else if (auto len = resp.contentLength(); len && status == 200) {
                expect = *len;
            }
        }
        if (expect > 0 && size < expect)
            return StepResult::retry("The connection closed before the download finished.", progressed);
        if (expect > 0 && size > expect) {
            fs::removeFile(partPath);
            setPart(0, 0);
            return StepResult::restart();
        }
        return finalize(partPath, size);
    }

    // Keep the valid bytes for the next attempt.
    closePart(true);
    if (resp.error != http::Error::None) {
        if (isConnectivityError(resp.error)) return StepResult::unreachable(unreachableMessage(resp), progressed);
        return StepResult::retry(std::format("Download error ({}).", resp.describe()), progressed);
    }

    switch (status) {
        case 416:
            fs::removeFile(partPath);
            setPart(0, 0);
            return StepResult::restart();
        case 400: return StepResult::fatal("The server doesn't allow downloading this item.");
        case 401:
            m_.forgetClient(task_.info.serverId, task_.info.userId);
            return StepResult::fatal(sessionExpiredMessage());
        case 403: return StepResult::fatal(permissionMessage());
        case 404:
        case 410: return StepResult::fatal("This item is no longer available on the server.");
        // A reverse proxy in front of a server that is down.
        case 502:
        case 503:
        case 504:
            return StepResult::unreachable(std::format("The server is unavailable (HTTP {}).", status), progressed);
        default: return StepResult::retry(std::format("The server returned an error (HTTP {}).", status), progressed);
    }
}

StepResult JobRunner::finalize(const std::string& partPath, uint64_t size) {
    const std::string finalPath = dir_ + "/" + task_.info.fileName;
    const std::string backupPath = finalPath + ".old";
    auto finishError = [](int err) {
        return std::format("Couldn't finish the file on the SD card ({}).", err ? std::strerror(err) : "unknown error");
    };
    // Without a .part, attempt() found the finished file already in place.
    if (fs::exists(partPath)) {
        // Horizon's rename fails when the target exists: move an older copy aside first and only
        // drop it once the new file is in place. The .part is never deleted here.
        bool hasBackup = false;
        if (fs::exists(finalPath)) {
            if (fs::exists(backupPath)) fs::removeFile(backupPath);
            if (std::rename(finalPath.c_str(), backupPath.c_str()) != 0) {
                const int err = errno;
                log::error(kTag, "{}: cannot move {} aside: {}", job_->key, finalPath, std::strerror(err));
                return StepResult::fatal(finishError(err));
            }
            hasBackup = true;
        }
        if (std::rename(partPath.c_str(), finalPath.c_str()) != 0) {
            const int err = errno;
            log::error(kTag, "{}: cannot rename {}: {}", job_->key, partPath, std::strerror(err));
            if (hasBackup && std::rename(backupPath.c_str(), finalPath.c_str()) != 0)
                log::error(kTag, "{}: cannot restore {}: {}", job_->key, finalPath, std::strerror(errno));
            return StepResult::fatal(finishError(err));
        }
    }
    // Also clears a backup left behind by a crash.
    if (fs::exists(backupPath) && !fs::removeFile(backupPath))
        log::warn(kTag, "{}: could not delete {}", job_->key, backupPath);

    // The manifest goes to the card before the task counts as completed: it is what rebuilds the
    // queue when queue.json is lost. If it can't be written the task fails (retriable; the next
    // attempt finds the finished file and only redoes this step).
    const int64_t completedAt = detail::unixNow();
    std::string manifest;
    {
        std::lock_guard lock(m_.mutex);
        const Task* t = m_.queue.find(job_->key);
        if (!t) return StepResult::stopped();  // removed meanwhile: handleStop deletes the folder
        Task done = *t;
        if (!Queue::transition(done, State::Completed)) return StepResult::fatal("Unexpected download state.");
        done.info.bytesDone = size;
        done.info.bytesTotal = size;
        done.info.completedAt = completedAt;
        done.info.attempt = 0;
        manifest = detail::dumpJson(detail::taskToJson(done), 2);
    }
    const std::string manifestPath = dir_ + "/" + detail::kManifestFile;
    if (!fs::writeFileAtomic(manifestPath, manifest)) {
        log::warn(kTag, "{}: cannot write {} ({}); retrying", job_->key, manifestPath, std::strerror(errno));
        if (!fs::writeFileAtomic(manifestPath, manifest)) {
            const int err = errno;
            log::error(kTag, "{}: storage write failed: cannot write {} ({})", job_->key, manifestPath,
                       err ? std::strerror(err) : "unknown error");
            return StepResult::fatal(writeErrorMessage(err, 0));
        }
    }

    {
        std::lock_guard lock(m_.mutex);
        Task* t = m_.queue.find(job_->key);
        if (!t) return StepResult::stopped();
        if (!Queue::transition(*t, State::Completed)) return StepResult::fatal("Unexpected download state.");
        t->info.bytesDone = size;
        t->info.bytesTotal = size;
        t->info.completedAt = completedAt;
        t->info.attempt = 0;
        m_.emit(EventType::Updated, *t);
        m_.markDirty(true);
        m_.updateAwake();
    }
    m_.invalidateMeta(job_->key);
    log::info(kTag, "{}: completed ({})", job_->key, detail::formatBytes(size));
    return StepResult::done();
}

}  // namespace

void DownloadManager::Impl::runJob(const std::shared_ptr<Job>& job) {
    JobRunner runner(*this, job);
    runner.run();
}

}  // namespace karafin::downloads
