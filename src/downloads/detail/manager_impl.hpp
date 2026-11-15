#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "core/platform.hpp"
#include "core/thread.hpp"
#include "downloads/detail/limiter.hpp"
#include "downloads/detail/queue.hpp"
#include "downloads/manager.hpp"
#include "jellyfin/client.hpp"

namespace karafin::downloads {

namespace detail {

enum class StopReason : int { None, Pause, Remove, Shutdown };

// One running transfer, owned by a dl-N thread.
struct Job {
    std::string key;
    std::string serverId;
    std::string itemId;
    uint64_t id = 0;
    CancelToken token;
    std::atomic<StopReason> reason{StopReason::None};
};

// Parsed metadata.json of a completed download (cached).
struct StoredMeta {
    jellyfin::BaseItem item;
    jellyfin::BaseItem series;
    jellyfin::BaseItem season;
    bool hasItem = false;
    bool hasSeries = false;
    bool hasSeason = false;
};

}  // namespace detail

struct DownloadManager::Impl {
    using Clock = std::chrono::steady_clock;

    // --- Queue state (mutex) ---
    mutable std::mutex mutex;
    std::condition_variable cv;  // slots wait for work; backoff waits wake on pause/shutdown
    detail::Queue queue;
    bool loaded = false;
    bool started = false;
    bool stopping = false;
    bool finished = false;  // shutdown() ran; only an explicit start() starts threads again
    std::map<std::string, std::shared_ptr<detail::Job>> jobs;  // key -> running job
    std::set<std::string> deleting;                            // folders scheduled for deletion
    uint64_t nextJobId = 1;
    std::optional<platform::AwakeLock> awake;
    bool awakeScreenOn = true;  // the mode `awake` was taken with
    bool dirty = false;
    bool dirtyUrgent = false;
    Clock::time_point dirtySince{};
    std::map<std::string, Clock::time_point> retryAt;  // WaitingForNetwork deadlines

    std::vector<Thread> slots;

    // --- I/O thread ---
    Thread ioThread;
    std::mutex ioMutex;
    std::condition_variable ioCv;
    std::deque<std::function<void()>> ioTasks;
    bool ioStop = false;
    std::mutex saveMutex;  // serializes queue.json writes

    // --- Listeners (main thread) ---
    std::vector<std::pair<int, Listener>> listeners;
    int nextListenerId = 1;

    // --- Events waiting for the main thread (eventMutex; taken under mutex) ---
    std::mutex eventMutex;
    std::vector<Event> pendingEvents;
    bool eventFlushPosted = false;

    // --- Clients per server/user ---
    std::mutex clientMutex;
    std::map<std::string, jellyfin::ClientPtr> clients;

    // --- Throttle ---
    detail::BandwidthLimiter limiter;
    std::atomic<bool> throttled{false};

    // --- Metadata cache ---
    mutable std::mutex metaMutex;
    mutable std::map<std::string, std::shared_ptr<const detail::StoredMeta>> metaCache;

    // --- manager.cpp ---
    void ensureLoaded();                 // mutex held
    void startLocked();                  // mutex held
    void ensureSlotsLocked();            // mutex held; one dl-N thread per allowed transfer
    void markDirty(bool urgent);         // mutex held
    void saveNow();                      // takes mutex briefly for the snapshot
    void postIo(std::function<void()> fn);
    void ioLoop();
    void slotLoop();
    void updateAwake();                  // mutex held
    DownloadInfo snapshot(const detail::Task& t) const;  // mutex held
    void emit(EventType type, const detail::Task& t);    // mutex held
    void emitInfo(EventType type, DownloadInfo info);
    void flushEvents();                  // main thread
    void dispatch(const Event& ev);      // main thread
    bool removeLocked(const std::string& key, bool allowCompleted);  // mutex held
    void scheduleDelete(const std::string& serverId, const std::string& itemId);  // mutex held
    void requestStop(const std::string& key, detail::StopReason reason);           // mutex held
    void addItems(const jellyfin::ClientPtr& client, const std::vector<jellyfin::BaseItem>& items,
                  EnqueueResult& result);
    int concurrencySetting() const;
    jellyfin::ClientPtr clientFor(const std::string& serverId, const std::string& userId);
    void forgetClient(const std::string& serverId, const std::string& userId);

    // --- job.cpp ---
    void runJob(const std::shared_ptr<detail::Job>& job);

    // --- library.cpp ---
    std::shared_ptr<const detail::StoredMeta> loadMeta(const detail::Task& t) const;
    void invalidateMeta(const std::string& key) const;
    OfflineItem makeOfflineItem(const detail::Task& t) const;
    void patchMetadataFile(const std::string& serverId, const std::string& itemId, int64_t positionTicks,
                           bool played);
    void queueReports(const detail::Task& t, int64_t positionTicks, bool reportPlayed, bool played);
};

}  // namespace karafin::downloads
