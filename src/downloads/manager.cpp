#include "downloads/manager.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>

#include <dirent.h>
#include <unistd.h>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/paths.hpp"
#include "core/settings.hpp"
#include "downloads/detail/manager_impl.hpp"
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
constexpr double kUrgentSaveDelay = 0.2;   // seconds after a state change
constexpr double kProgressSaveDelay = 10;  // seconds while only bytes change

std::vector<std::string> subdirectories(const std::string& path) {
    std::vector<std::string> out;
    if (DIR* dir = opendir(path.c_str())) {
        while (dirent* ent = readdir(dir)) {
            const std::string name = ent->d_name;
            if (name == "." || name == ".." || name.starts_with(".")) continue;
            if (fs::isDirectory(path + "/" + name)) out.push_back(name);
        }
        closedir(dir);
    }
    return out;
}

std::string permissionMessage() {
    http::Response forbidden;
    forbidden.status = 403;
    return jellyfin::ApiError::fromResponse(forbidden, jellyfin::ErrorContext::Download).message;
}

// fs::readFileAtomic() that also tells which file the text came from (<path> or <path>.tmp).
struct LoadedFile {
    std::string text;
    std::string path;
};
std::optional<LoadedFile> readStateFile(const std::string& path) {
    if (auto text = fs::readFile(path)) return LoadedFile{std::move(*text), path};
    const std::string tmp = path + ".tmp";
    if (auto text = fs::readFile(tmp)) return LoadedFile{std::move(*text), tmp};
    return std::nullopt;
}

// Moves a damaged file to <file>.corrupt (replacing an older one): kept for inspection, but not
// parsed again on every start. `path` is the logical name, `actual` the file that was read.
void quarantine(const std::string& path, const std::string& actual) {
    const std::string target = path + ".corrupt";
    if (fs::exists(target)) fs::removeFile(target);
    if (std::rename(actual.c_str(), target.c_str()) == 0) {
        log::warn(kTag, "moved damaged {} to {}", actual, target);
    } else {
        log::error(kTag, "could not move damaged {} aside: {}", actual, std::strerror(errno));
    }
}

bool isDownloadable(const jellyfin::BaseItem& item) {
    if (item.isFolder || item.isVirtual() || item.id.empty()) return false;
    return item.isMovie() || item.isEpisode() || item.type == "Video" || item.type == "MusicVideo" ||
           item.mediaType == "Video";
}

Task taskFromItem(const jellyfin::ClientPtr& client, const jellyfin::BaseItem& it) {
    Task t;
    DownloadInfo& i = t.info;
    i.serverId = it.serverId.empty() ? client->serverId() : it.serverId;
    i.userId = client->userId();
    i.itemId = it.id;
    i.key = makeKey(i.serverId, i.itemId);
    const jellyfin::MediaSource* ms = it.mediaSource("");
    i.mediaSourceId = ms ? ms->id : std::string();
    i.type = it.type;
    i.name = it.name;
    i.seriesId = it.seriesId;
    i.seriesName = it.seriesName;
    i.seasonId = it.seasonId;
    i.seasonName = it.seasonName;
    i.indexNumber = it.indexNumber;
    i.parentIndexNumber = it.parentIndexNumber;
    i.productionYear = it.productionYear;
    i.runTimeTicks = it.runTimeTicks;
    i.container = ms && !ms->container.empty() ? ms->container : it.container;
    i.overview = it.overview;
    const int64_t size = ms && ms->size > 0 ? ms->size : it.size;
    i.bytesTotal = size > 0 ? static_cast<uint64_t>(size) : 0;
    i.positionTicks = it.userData.playbackPositionTicks;
    i.played = it.userData.played;
    i.addedAt = detail::unixNow();
    i.state = State::Queued;
    return t;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Impl

void DownloadManager::Impl::ensureLoaded() {
    if (loaded) return;
    loaded = true;
    fs::ensureDir(paths::downloadsDir());
    const bool autoRes = Settings::instance().get("downloads.auto_resume", false);

    bool ok = false;
    if (auto file = readStateFile(detail::queueFile())) {
        const auto doc = nlohmann::json::parse(file->text, nullptr, false);
        ok = !doc.is_discarded() && queue.fromJson(doc, autoRes);
        if (!ok) {
            log::warn(kTag, "{} is invalid; rebuilding from the item folders", file->path);
            quarantine(detail::queueFile(), file->path);
        }
    }
    if (!ok) {
        // Lost / corrupt queue: recover completed downloads from their manifests.
        int recovered = 0;
        for (const auto& server : subdirectories(paths::downloadsDir())) {
            const std::string sdir = detail::serverDir(server);
            for (const auto& item : subdirectories(sdir)) {
                const std::string manifestPath = sdir + "/" + item + "/" + detail::kManifestFile;
                auto file = readStateFile(manifestPath);
                if (!file) continue;
                const auto doc = nlohmann::json::parse(file->text, nullptr, false);
                std::optional<Task> task;
                if (!doc.is_discarded()) task = detail::taskFromJson(doc);
                if (!task) {
                    log::warn(kTag, "{} is damaged; skipping it", file->path);
                    quarantine(manifestPath, file->path);
                    continue;
                }
                if (task->info.state != State::Completed) continue;
                if (queue.add(std::move(*task))) ++recovered;
            }
        }
        if (recovered > 0) {
            log::info(kTag, "recovered {} completed downloads from manifests", recovered);
            markDirty(true);
        }
    }

    // The files on disk are the truth for sizes.
    for (auto& t : queue.tasks()) {
        DownloadInfo& i = t.info;
        const std::string dir = detail::itemDir(i.serverId, i.itemId);
        if (i.state == State::Completed) {
            const auto size = i.fileName.empty() ? std::nullopt : fs::fileSize(dir + "/" + i.fileName);
            if (!size) {
                Queue::transition(t, State::Failed,
                                  "The downloaded file is missing from the SD card. Retry to download it again.");
                i.bytesDone = 0;
                t.assetsDone = false;
                markDirty(true);
            } else {
                i.bytesDone = *size;
                if (i.bytesTotal == 0) i.bytesTotal = *size;
            }
        } else {
            const uint64_t partSize =
                i.fileName.empty() ? 0 : fs::fileSize(dir + "/" + i.fileName + detail::kPartSuffix).value_or(0);
            i.bytesDone = detail::resumeOffset(partSize, t.syncedBytes);
        }
    }
    log::info(kTag, "queue loaded: {} items", queue.size());
}

void DownloadManager::Impl::startLocked() {
    ensureLoaded();
    // After shutdown() (AppDelegate::onStop) the HTTP client goes away: late calls from screens
    // being torn down must not spawn transfer threads again.
    if (started || finished) return;
    started = true;
    stopping = false;
    {
        std::lock_guard io(ioMutex);
        ioStop = false;
    }
    ThreadOptions ioOpts;
    ioOpts.name = "dl-io";
    ioOpts.stackSize = 256 * 1024;
    ioOpts.priority = kPriorityBackground;
    ioThread = Thread(std::move(ioOpts), [this] { ioLoop(); });
    ensureSlotsLocked();
    log::info(kTag, "started (concurrency {}, auto-resume {})", concurrencySetting(),
              Settings::instance().get("downloads.auto_resume", false));
    cv.notify_all();
}

void DownloadManager::Impl::ensureSlotsLocked() {
    // Threads are only added: after the setting is lowered the extra ones stay parked in
    // slotLoop, which never runs more than concurrencySetting() jobs at once.
    if (!started || stopping) return;
    const auto wanted = static_cast<size_t>(concurrencySetting());
    while (slots.size() < wanted) {
        ThreadOptions opts;
        opts.name = "dl-" + std::to_string(slots.size() + 1);
        opts.stackSize = 512 * 1024;  // mbedtls handshakes + JSON parsing
        opts.priority = kPriorityIo;
        slots.emplace_back(std::move(opts), [this] { slotLoop(); });
    }
}

void DownloadManager::Impl::markDirty(bool urgent) {
    const auto now = Clock::now();
    if (!dirty) dirtySince = now;
    dirty = true;
    if (urgent && !dirtyUrgent) {
        dirtyUrgent = true;
        dirtySince = now;
    }
}

void DownloadManager::Impl::saveNow() {
    nlohmann::json doc;
    {
        std::lock_guard lock(mutex);
        if (!loaded) return;
        doc = queue.toJson();
        dirty = false;
        dirtyUrgent = false;
    }
    std::lock_guard save(saveMutex);
    if (!fs::writeFileAtomic(detail::queueFile(), detail::dumpJson(doc))) {
        log::error(kTag, "cannot write {}", detail::queueFile());
        std::lock_guard lock(mutex);
        markDirty(false);
    }
}

void DownloadManager::Impl::postIo(std::function<void()> fn) {
    {
        std::lock_guard lock(ioMutex);
        ioTasks.push_back(std::move(fn));
    }
    ioCv.notify_one();
}

void DownloadManager::Impl::ioLoop() {
    for (;;) {
        std::function<void()> task;
        bool exit = false;
        {
            std::unique_lock lock(ioMutex);
            ioCv.wait_for(lock, std::chrono::milliseconds(250), [this] { return ioStop || !ioTasks.empty(); });
            if (!ioTasks.empty()) {
                task = std::move(ioTasks.front());
                ioTasks.pop_front();
            } else if (ioStop) {
                exit = true;
            }
        }
        if (exit) break;
        if (task) task();

        bool save = false;
        {
            std::lock_guard lock(mutex);
            if (dirty) {
                const double age = std::chrono::duration<double>(Clock::now() - dirtySince).count();
                save = age >= (dirtyUrgent ? kUrgentSaveDelay : kProgressSaveDelay);
            }
        }
        if (save) saveNow();
    }
}

int DownloadManager::Impl::concurrencySetting() const {
    return std::clamp(Settings::instance().get("downloads.concurrency", 1), 1, DownloadManager::kMaxConcurrency);
}

void DownloadManager::Impl::slotLoop() {
    std::unique_lock lock(mutex);
    while (!stopping) {
        Task* t = nullptr;
        if (queue.runningCount() < concurrencySetting()) {
            t = queue.nextRunnable(
                [this](const Task& c) { return jobs.count(c.info.key) > 0 || deleting.count(c.info.key) > 0; });
        }
        if (!t) {
            cv.wait_for(lock, std::chrono::seconds(5));
            continue;
        }
        auto job = std::make_shared<Job>();
        job->key = t->info.key;
        job->serverId = t->info.serverId;
        job->itemId = t->info.itemId;
        job->id = nextJobId++;
        t->running = true;
        jobs[job->key] = job;
        lock.unlock();

        try {
            runJob(job);
        } catch (const std::exception& e) {
            log::error(kTag, "job {} crashed: {}", job->key, e.what());
            std::lock_guard relock(mutex);
            if (Task* ct = queue.find(job->key); ct && ct->info.state != State::Completed &&
                                                 job->reason.load() == StopReason::None) {
                Queue::transition(*ct, State::Failed, "Unexpected error while downloading.");
                emit(EventType::Updated, *ct);
                markDirty(true);
            }
        }

        lock.lock();
        if (auto it = jobs.find(job->key); it != jobs.end() && it->second == job) jobs.erase(it);
        if (Task* done = queue.find(job->key)) done->running = false;
        updateAwake();
        cv.notify_all();
    }
}

void DownloadManager::Impl::updateAwake() {
    const bool any = std::any_of(queue.tasks().begin(), queue.tasks().end(), [](const Task& t) {
        return t.running && (t.info.state == State::Probing || t.info.state == State::Downloading);
    });
    const bool screenOn = Settings::instance().get("downloads.keep_screen_on", true);
    if (!any) {
        awake.reset();
        return;
    }
    if (awake && awakeScreenOn == screenOn) return;
    // A bridging lock keeps auto-sleep off while the old lock is swapped for the new mode.
    platform::AwakeLock bridge(false);
    awake.reset();
    awake.emplace(screenOn);
    awakeScreenOn = screenOn;
}

DownloadInfo DownloadManager::Impl::snapshot(const Task& t) const {
    DownloadInfo info = t.info;
    if (info.state == State::WaitingForNetwork) {
        if (auto it = retryAt.find(info.key); it != retryAt.end()) {
            info.retryInSeconds = std::max(0.0, std::chrono::duration<double>(it->second - Clock::now()).count());
        }
    }
    return info;
}

void DownloadManager::Impl::emit(EventType type, const Task& t) { emitInfo(type, snapshot(t)); }

// Usually called with `mutex` held, so it must not block on the main queue. Events are buffered
// and delivered by a single outstanding flush task; a newer Progress event replaces a pending one
// for the same download, so the buffer stays bounded by the number of downloads and state changes.
void DownloadManager::Impl::emitInfo(EventType type, DownloadInfo info) {
    Event ev;
    ev.type = type;
    ev.info = std::move(info);
    std::lock_guard lock(eventMutex);
    if (type == EventType::Progress) {
        for (auto it = pendingEvents.rbegin(); it != pendingEvents.rend(); ++it) {
            if (it->info.key != ev.info.key) continue;
            if (it->type == EventType::Progress) {
                *it = std::move(ev);
                return;
            }
            break;
        }
    }
    pendingEvents.push_back(std::move(ev));
    if (eventFlushPosted) return;
    eventFlushPosted = true;
    main_thread::postNoWait([this] { flushEvents(); });
}

void DownloadManager::Impl::flushEvents() {
    std::vector<Event> batch;
    {
        std::lock_guard lock(eventMutex);
        batch.swap(pendingEvents);
        eventFlushPosted = false;
    }
    for (const auto& ev : batch) dispatch(ev);
}

void DownloadManager::Impl::dispatch(const Event& ev) {
    const auto copy = listeners;
    for (const auto& [id, fn] : copy) {
        // A listener may unsubscribe others (e.g. by popping their screen) while we iterate.
        const bool still = std::any_of(listeners.begin(), listeners.end(), [id = id](const auto& l) { return l.first == id; });
        if (still && fn) fn(ev);
    }
}

void DownloadManager::Impl::requestStop(const std::string& key, StopReason reason) {
    auto it = jobs.find(key);
    if (it == jobs.end()) return;
    auto& job = *it->second;
    StopReason expected = StopReason::None;
    if (!job.reason.compare_exchange_strong(expected, reason) && reason == StopReason::Remove) {
        job.reason.store(StopReason::Remove);  // cancel overrides an earlier pause
    }
    job.token.cancel();
    cv.notify_all();
}

void DownloadManager::Impl::scheduleDelete(const std::string& serverId, const std::string& itemId) {
    if (serverId.empty() || itemId.empty()) return;  // never resolve to the downloads root
    const std::string key = makeKey(serverId, itemId);
    deleting.insert(key);
    postIo([this, key, serverId, itemId] {
        const std::string dir = detail::itemDir(serverId, itemId);
        if (!fs::removeTree(dir)) log::warn(kTag, "could not delete {}", dir);
        rmdir(detail::serverDir(serverId).c_str());  // only succeeds when empty
        std::lock_guard lock(mutex);
        deleting.erase(key);
        cv.notify_all();
    });
}

bool DownloadManager::Impl::removeLocked(const std::string& key, bool allowCompleted) {
    Task* t = queue.find(key);
    if (!t) return false;
    if (!allowCompleted && t->info.state == State::Completed) return false;
    DownloadInfo last = snapshot(*t);
    const std::string serverId = t->info.serverId;
    const std::string itemId = t->info.itemId;
    if (jobs.count(key)) {
        requestStop(key, StopReason::Remove);  // the job deletes the folder when it stops
    } else {
        scheduleDelete(serverId, itemId);
    }
    queue.remove(key);
    retryAt.erase(key);
    invalidateMeta(key);
    emitInfo(EventType::Removed, std::move(last));
    markDirty(true);
    updateAwake();
    log::info(kTag, "removed {}", key);
    return true;
}

void DownloadManager::Impl::addItems(const jellyfin::ClientPtr& client, const std::vector<jellyfin::BaseItem>& items,
                                     EnqueueResult& result) {
    std::lock_guard lock(mutex);
    startLocked();
    for (const auto& it : items) {
        if (!isDownloadable(it)) {
            ++result.skipped;
            continue;
        }
        Task task = taskFromItem(client, it);
        if (task.info.serverId.empty()) {
            ++result.skipped;
            continue;
        }
        const std::string key = task.info.key;
        if (Task* existing = queue.find(key)) {
            if (Queue::retry(*existing)) {
                ++result.added;
                result.keys.push_back(key);
                emit(EventType::Updated, *existing);
            } else {
                ++result.skipped;
            }
            continue;
        }
        if (!queue.add(std::move(task))) {
            ++result.skipped;
            continue;
        }
        ++result.added;
        result.keys.push_back(key);
        if (const Task* added = queue.find(key)) emit(EventType::Added, *added);
    }
    if (result.added > 0) {
        markDirty(true);
        cv.notify_all();
        log::info(kTag, "queued {} item(s), skipped {}", result.added, result.skipped);
    }
}

jellyfin::ClientPtr DownloadManager::Impl::clientFor(const std::string& serverId, const std::string& userId) {
    if (auto c = jellyfin::currentClient();
        c && c->isAuthenticated() && c->serverId() == serverId && (userId.empty() || c->userId() == userId)) {
        return c;
    }
    auto server = jellyfin::AccountStore::instance().server(serverId);
    if (!server) return nullptr;
    const jellyfin::StoredUser* user = server->user(userId);
    if (!user || !user->hasToken()) return nullptr;
    std::lock_guard lock(clientMutex);
    const std::string k = makeKey(serverId, userId);
    if (auto it = clients.find(k); it != clients.end() && it->second->accessToken() == user->token) return it->second;
    auto c = jellyfin::Client::fromStored(*server, user);
    clients[k] = c;
    return c;
}

void DownloadManager::Impl::forgetClient(const std::string& serverId, const std::string& userId) {
    std::lock_guard lock(clientMutex);
    clients.erase(makeKey(serverId, userId));
}

// ------------------------------------------------------------------------------------------------
// DownloadManager

DownloadManager& DownloadManager::instance() {
    static DownloadManager m;
    return m;
}

DownloadManager::DownloadManager() : impl_(std::make_unique<Impl>()) {}

// Static destruction runs after log::shutdown(): no logging here. shutdown() normally ran in
// onStop already; this only persists changes made after it.
DownloadManager::~DownloadManager() {
    bool running = false;
    bool dirty = false;
    {
        std::lock_guard lock(impl_->mutex);
        running = impl_->started;
        dirty = impl_->loaded && impl_->dirty;
    }
    if (running) {
        shutdown();
    } else if (dirty) {
        impl_->saveNow();
    }
}

void DownloadManager::start() {
    std::lock_guard lock(impl_->mutex);
    impl_->finished = false;
    impl_->startLocked();
}

void DownloadManager::shutdown() {
    Impl& m = *impl_;
    {
        std::lock_guard lock(m.mutex);
        m.finished = true;
        if (!m.started) {
            if (!m.loaded || !m.dirty) return;
        } else {
            m.stopping = true;
            for (auto& [key, job] : m.jobs) {
                StopReason expected = StopReason::None;
                job->reason.compare_exchange_strong(expected, StopReason::Shutdown);
                job->token.cancel();
            }
            m.cv.notify_all();
        }
    }
    if (!m.slots.empty()) {
        for (auto& s : m.slots) s.join();
        m.slots.clear();
    }
    if (m.ioThread.joinable()) {
        {
            std::lock_guard io(m.ioMutex);
            m.ioStop = true;
        }
        m.ioCv.notify_all();
        m.ioThread.join();
        // Run what the I/O thread did not get to (deletions queued during shutdown).
        std::deque<std::function<void()>> rest;
        {
            std::lock_guard io(m.ioMutex);
            rest.swap(m.ioTasks);
        }
        for (auto& fn : rest) fn();
    }
    m.saveNow();
    std::lock_guard lock(m.mutex);
    m.started = false;
    m.stopping = false;
    m.awake.reset();
    log::info(kTag, "stopped");
}

bool DownloadManager::started() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->started;
}

void DownloadManager::enqueue(const jellyfin::BaseItem& item, EnqueueCallback done) {
    enqueue(jellyfin::currentClient(), item, std::move(done));
}

void DownloadManager::enqueue(jellyfin::ClientPtr client, const jellyfin::BaseItem& item, EnqueueCallback done) {
    Impl* m = impl_.get();
    auto finish = [done](EnqueueResult r) {
        if (done) main_thread::post([done, r = std::move(r)] { done(r); });
    };
    if (!client || !client->isAuthenticated()) {
        EnqueueResult r;
        r.error = "Sign in to a server to download.";
        return finish(std::move(r));
    }
    const bool expand = item.isSeries() || item.isSeason();
    if (!expand && !isDownloadable(item)) {
        EnqueueResult r;
        r.error = "This item can't be downloaded.";
        r.skipped = 1;
        return finish(std::move(r));
    }

    client->canDownload().then([m, client, item, expand, finish](jellyfin::Result<bool> allowed) {
        // Offline: trust the cached policy; the job waits for the network anyway.
        const bool ok = allowed ? *allowed : client->user().canDownload();
        if (!ok) {
            EnqueueResult r;
            r.error = permissionMessage();
            return finish(std::move(r));
        }
        if (!expand) {
            EnqueueResult r;
            m->addItems(client, {item}, r);
            if (r.added == 0 && r.error.empty()) r.error = "Already downloaded or in the queue.";
            return finish(std::move(r));
        }
        const std::string seriesId = item.isSeries() ? item.id : item.seriesId;
        const std::string seasonId = item.isSeason() ? item.id : std::string();
        client->episodes(seriesId, seasonId).then([m, client, finish](jellyfin::Result<jellyfin::QueryResult<jellyfin::BaseItem>> eps) {
            EnqueueResult r;
            if (!eps) {
                r.error = eps.error.message.empty() ? "Couldn't load the episodes." : eps.error.message;
                return finish(std::move(r));
            }
            m->addItems(client, eps->items, r);
            if (r.added == 0 && r.error.empty())
                r.error = r.skipped > 0 ? "All episodes are already downloaded or queued." : "No episodes to download.";
            finish(std::move(r));
        });
    });
}

bool DownloadManager::pause(const std::string& key) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.ensureLoaded();
    Task* t = m.queue.find(key);
    if (!t || !Queue::pause(*t)) return false;
    m.requestStop(key, StopReason::Pause);
    m.retryAt.erase(key);
    m.emit(EventType::Updated, *t);
    m.markDirty(true);
    m.updateAwake();
    return true;
}

bool DownloadManager::resume(const std::string& key) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.startLocked();
    Task* t = m.queue.find(key);
    if (!t || !Queue::resume(*t)) return false;
    m.emit(EventType::Updated, *t);
    m.markDirty(true);
    m.cv.notify_all();
    return true;
}

void DownloadManager::pauseAll() {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.ensureLoaded();
    bool any = false;
    for (auto& t : m.queue.tasks()) {
        if (!Queue::pause(t)) continue;
        m.requestStop(t.info.key, StopReason::Pause);
        m.retryAt.erase(t.info.key);
        m.emit(EventType::Updated, t);
        any = true;
    }
    if (any) {
        m.markDirty(true);
        m.updateAwake();
    }
}

void DownloadManager::resumeAll() {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.startLocked();
    bool any = false;
    for (auto& t : m.queue.tasks()) {
        if (!Queue::resume(t)) continue;
        m.emit(EventType::Updated, t);
        any = true;
    }
    if (any) {
        m.markDirty(true);
        m.cv.notify_all();
    }
}

bool DownloadManager::retry(const std::string& key) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.startLocked();
    Task* t = m.queue.find(key);
    if (!t || !Queue::retry(*t)) return false;
    m.emit(EventType::Updated, *t);
    m.markDirty(true);
    m.cv.notify_all();
    return true;
}

int DownloadManager::retryFailed() {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.startLocked();
    int n = 0;
    for (auto& t : m.queue.tasks()) {
        if (!Queue::retry(t)) continue;
        m.emit(EventType::Updated, t);
        ++n;
    }
    if (n > 0) {
        m.markDirty(true);
        m.cv.notify_all();
    }
    return n;
}

bool DownloadManager::cancel(const std::string& key) {
    std::lock_guard lock(impl_->mutex);
    impl_->startLocked();
    return impl_->removeLocked(key, false);
}

bool DownloadManager::remove(const std::string& key) {
    std::lock_guard lock(impl_->mutex);
    impl_->startLocked();
    return impl_->removeLocked(key, true);
}

int DownloadManager::removeGroup(const std::string& serverId, const std::string& parentId) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.startLocked();
    std::vector<std::string> keys;
    for (const auto& t : m.queue.tasks()) {
        if (!serverId.empty() && t.info.serverId != serverId) continue;
        if (t.info.seriesId == parentId || t.info.seasonId == parentId) keys.push_back(t.info.key);
    }
    int n = 0;
    for (const auto& k : keys) n += m.removeLocked(k, true) ? 1 : 0;
    return n;
}

int DownloadManager::removeWatched() {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.startLocked();
    std::vector<std::string> keys;
    for (const auto& t : m.queue.tasks())
        if (t.info.state == State::Completed && t.info.played) keys.push_back(t.info.key);
    int n = 0;
    for (const auto& k : keys) n += m.removeLocked(k, true) ? 1 : 0;
    if (n > 0) log::info(kTag, "deleted {} watched download(s)", n);
    return n;
}

void DownloadManager::setConcurrency(int n) {
    Settings::instance().set("downloads.concurrency", std::clamp(n, 1, DownloadManager::kMaxConcurrency));
    std::lock_guard lock(impl_->mutex);
    impl_->ensureSlotsLocked();
    impl_->cv.notify_all();
}

int DownloadManager::concurrency() const { return impl_->concurrencySetting(); }

void DownloadManager::setAutoResume(bool enabled) { Settings::instance().set("downloads.auto_resume", enabled); }

bool DownloadManager::autoResume() const { return Settings::instance().get("downloads.auto_resume", false); }

void DownloadManager::setKeepScreenOn(bool enabled) {
    Settings::instance().set("downloads.keep_screen_on", enabled);
    std::lock_guard lock(impl_->mutex);
    impl_->updateAwake();
}

bool DownloadManager::keepScreenOn() const { return Settings::instance().get("downloads.keep_screen_on", true); }

void DownloadManager::setThrottled(bool throttled) {
    Impl& m = *impl_;
    if (m.throttled.exchange(throttled) == throttled) return;
    const int kbps = std::max(128, Settings::instance().get("downloads.throttle_kbps", 1024));
    m.limiter.setLimit(throttled ? static_cast<uint64_t>(kbps) * 1024 : 0);
    log::info(kTag, "throttle {}", throttled ? std::to_string(kbps) + " KiB/s" : std::string("off"));
}

bool DownloadManager::throttled() const { return impl_->throttled.load(); }

std::vector<DownloadInfo> DownloadManager::list() const {
    std::lock_guard lock(impl_->mutex);
    impl_->ensureLoaded();
    std::vector<DownloadInfo> out;
    out.reserve(impl_->queue.size());
    for (const auto& t : impl_->queue.tasks()) out.push_back(impl_->snapshot(t));
    return out;
}

std::vector<DownloadInfo> DownloadManager::pending() const {
    std::lock_guard lock(impl_->mutex);
    impl_->ensureLoaded();
    std::vector<DownloadInfo> out;
    for (const auto& t : impl_->queue.tasks())
        if (t.info.state != State::Completed) out.push_back(impl_->snapshot(t));
    return out;
}

std::vector<DownloadInfo> DownloadManager::completed() const {
    std::vector<DownloadInfo> out;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->ensureLoaded();
        for (const auto& t : impl_->queue.tasks())
            if (t.info.state == State::Completed) out.push_back(t.info);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const DownloadInfo& a, const DownloadInfo& b) { return a.completedAt > b.completedAt; });
    return out;
}

std::optional<DownloadInfo> DownloadManager::find(const std::string& key) const {
    std::lock_guard lock(impl_->mutex);
    impl_->ensureLoaded();
    if (const Task* t = impl_->queue.find(key)) return impl_->snapshot(*t);
    return std::nullopt;
}

std::optional<DownloadInfo> DownloadManager::find(const std::string& serverId, const std::string& itemId) const {
    return find(makeKey(serverId, itemId));
}

std::optional<DownloadInfo> DownloadManager::find(const jellyfin::BaseItem& item) const {
    std::string serverId = item.serverId;
    if (serverId.empty())
        if (auto c = jellyfin::currentClient()) serverId = c->serverId();
    if (serverId.empty()) return findItem(item.id);
    return find(makeKey(serverId, item.id));
}

std::optional<DownloadInfo> DownloadManager::findItem(const std::string& itemId) const {
    std::lock_guard lock(impl_->mutex);
    impl_->ensureLoaded();
    if (const Task* t = impl_->queue.findItem(itemId)) return impl_->snapshot(*t);
    return std::nullopt;
}

bool DownloadManager::isDownloaded(const std::string& itemId) const {
    std::lock_guard lock(impl_->mutex);
    impl_->ensureLoaded();
    for (const auto& t : impl_->queue.tasks())
        if (t.info.itemId == itemId && t.info.state == State::Completed) return true;
    return false;
}

GroupSummary DownloadManager::summary(const std::string& serverId, const std::string& parentId) const {
    GroupSummary s;
    if (parentId.empty()) return s;
    std::lock_guard lock(impl_->mutex);
    impl_->ensureLoaded();
    for (const auto& t : impl_->queue.tasks()) {
        const DownloadInfo& i = t.info;
        if (!serverId.empty() && i.serverId != serverId) continue;
        if (i.seriesId != parentId && i.seasonId != parentId) continue;
        ++s.total;
        switch (i.state) {
            case State::Completed: ++s.completed; break;
            case State::Queued: ++s.queued; break;
            case State::Paused: ++s.paused; break;
            case State::Failed: ++s.failed; break;
            default: ++s.active; break;
        }
        s.bytesDone += i.state == State::Completed ? std::max(i.bytesDone, i.bytesTotal) : i.bytesDone;
        s.bytesTotal += std::max(i.bytesDone, i.bytesTotal);
    }
    return s;
}

StorageStats DownloadManager::storage() const {
    StorageStats s;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->ensureLoaded();
        for (const auto& t : impl_->queue.tasks()) {
            const DownloadInfo& i = t.info;
            if (i.state == State::Completed) {
                s.downloadedBytes += i.bytesDone;
                ++s.completedCount;
            } else {
                s.partialBytes += i.bytesDone;
                if (i.bytesTotal > i.bytesDone) s.reservedBytes += i.bytesTotal - i.bytesDone;
                ++s.pendingCount;
            }
        }
    }
    if (auto sp = fs::space(paths::downloadsDir())) {
        s.freeBytes = sp->freeBytes;
        s.totalBytes = sp->totalBytes;
    }
    return s;
}

bool DownloadManager::hasActive() const {
    std::lock_guard lock(impl_->mutex);
    return std::any_of(impl_->queue.tasks().begin(), impl_->queue.tasks().end(),
                       [](const Task& t) { return isActive(t.info.state); });
}

int DownloadManager::subscribe(Listener listener) {
    main_thread::assertCurrent("downloads::subscribe");
    const int id = impl_->nextListenerId++;
    impl_->listeners.emplace_back(id, std::move(listener));
    return id;
}

void DownloadManager::unsubscribe(int id) {
    main_thread::assertCurrent("downloads::unsubscribe");
    std::erase_if(impl_->listeners, [id](const auto& l) { return l.first == id; });
}

std::string DownloadManager::itemDirectory(const std::string& serverId, const std::string& itemId) {
    return detail::itemDir(serverId, itemId);
}

}  // namespace karafin::downloads
