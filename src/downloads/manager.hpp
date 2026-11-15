#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "downloads/types.hpp"
#include "jellyfin/client.hpp"

namespace karafin::downloads {

// Offline downloads: persistent queue, transfers with pause / resume (HTTP Range into
// <file>.part), retry with backoff, bundling of metadata / images / subtitles, the offline
// library and local playback progress. See src/downloads/README.md.
//
// Threading: every method is thread-safe. enqueue() and subscribe() are meant for the main
// thread; their callbacks and all events run on the main thread. Transfers run on dedicated
// "dl-N" threads (blocking http::Client::perform), file deletions and queue saves on "dl-io".
class DownloadManager {
public:
    static constexpr int kMaxConcurrency = 4;
    static DownloadManager& instance();

    // --- Lifecycle -----------------------------------------------------------------------------
    // Loads queue.json (also done lazily by the first call of any method), restores unfinished
    // downloads as Paused (or Queued with downloads.auto_resume) and starts the threads. Call
    // once the App runs (AppDelegate::onStart); control methods call it lazily too.
    void start();
    // Stops the transfers (they keep their .part and come back on next launch), saves the
    // queue and joins the threads. Call from AppDelegate::onStop, before the HTTP client stops.
    void shutdown();
    bool started() const;

    // --- Enqueue (main thread) -----------------------------------------------------------------
    using EnqueueCallback = std::function<void(EnqueueResult)>;
    // Movie / Episode / Video: one download. Season / Series: every non-virtual episode (via
    // client->episodes()). Checks the "Allow media downloading" permission first. Items already
    // queued or downloaded are skipped; Failed ones are re-queued. `done` runs on the main thread.
    void enqueue(const jellyfin::BaseItem& item, EnqueueCallback done = {});  // uses currentClient()
    void enqueue(jellyfin::ClientPtr client, const jellyfin::BaseItem& item, EnqueueCallback done = {});

    // --- Control (keys from DownloadInfo::key / makeKey) ----------------------------------------
    bool pause(const std::string& key);   // Queued / active -> Paused (keeps the .part)
    bool resume(const std::string& key);  // Paused -> Queued
    void pauseAll();
    void resumeAll();
    bool retry(const std::string& key);   // Failed -> Queued
    int retryFailed();
    // Unfinished download: stops it, deletes the partial files and removes it. False if the
    // download is Completed (use remove()) or unknown.
    bool cancel(const std::string& key);
    // Any state: stops it if needed, deletes the item folder and forgets it.
    bool remove(const std::string& key);
    // Removes every download (any state) of a series or season (parentId = seriesId / seasonId).
    int removeGroup(const std::string& serverId, const std::string& parentId);
    // Deletes completed downloads that are marked played locally. Returns how many.
    int removeWatched();

    // --- Settings ------------------------------------------------------------------------------
    // Concurrent transfers, 1..4 (setting downloads.concurrency, default 1). Main thread.
    void setConcurrency(int n);
    int concurrency() const;
    // Resume interrupted downloads automatically on launch (downloads.auto_resume, default false).
    void setAutoResume(bool enabled);
    bool autoResume() const;
    // Bandwidth cap while a streamed video plays: the player calls setThrottled(true) when it
    // starts streaming from the server and false when it stops (not for local files). The cap
    // is downloads.throttle_kbps KiB/s (default 1024, minimum 128) across all transfers.
    void setThrottled(bool throttled);
    bool throttled() const;

    // --- Snapshots (thread-safe, cheap copies) ---------------------------------------------------
    std::vector<DownloadInfo> list() const;       // all, queue order
    std::vector<DownloadInfo> pending() const;    // not Completed, queue order
    std::vector<DownloadInfo> completed() const;  // most recently completed first
    std::optional<DownloadInfo> find(const std::string& key) const;
    std::optional<DownloadInfo> find(const std::string& serverId, const std::string& itemId) const;
    // Detail screens: by BaseItem (its serverId, else the current client's server).
    std::optional<DownloadInfo> find(const jellyfin::BaseItem& item) const;
    std::optional<DownloadInfo> findItem(const std::string& itemId) const;  // any server
    bool isDownloaded(const std::string& itemId) const;                     // Completed, any server
    // All downloads below a series or season (parentId = seriesId or seasonId).
    GroupSummary summary(const std::string& serverId, const std::string& parentId) const;
    StorageStats storage() const;  // statvfs of the SD card + per-item sizes
    bool hasActive() const;        // any Probing / Downloading / WaitingForNetwork

    // --- Offline library (completed downloads) --------------------------------------------------
    // Reads metadata.json of each item the first time (cached afterwards): prefer
    // loadLibrary() from the main thread. serverId "" = all servers.
    OfflineLibrary library(const std::string& serverId = {}) const;
    // Builds the library on workers() and delivers it on the main thread.
    void loadLibrary(std::function<void(OfflineLibrary)> done, const std::string& serverId = {}) const;
    // Everything the player needs for one completed download (media path, subtitles, saved
    // position, parsed item). nullopt unless Completed.
    std::optional<OfflineItem> offlineItem(const std::string& key) const;
    // Next downloaded episode of the same series after `key` (offline autoplay).
    std::optional<OfflineItem> nextOfflineEpisode(const std::string& key) const;

    // --- Local playback progress -----------------------------------------------------------------
    // Saves the position of a downloaded item (queue.json + metadata.json UserData). `finished`
    // (or >= 90% watched) marks it played and clears the position. With `queueServerReport`
    // the position is queued in jellyfin::ReportQueue (Stopped + Played) and flushed when the
    // server is reachable - pass false when the player already reported it online.
    void saveLocalProgress(const std::string& key, int64_t positionTicks, bool finished = false,
                           bool queueServerReport = true);
    // Mark watched / unwatched offline (clears the position).
    void setLocalPlayed(const std::string& key, bool played, bool queueServerReport = true);

    // --- Observer (main thread) ------------------------------------------------------------------
    using Listener = std::function<void(const Event&)>;
    int subscribe(Listener listener);
    void unsubscribe(int id);

    // Absolute item folder (sdmc:/switch/karafin/downloads/<serverId>/<itemId>).
    static std::string itemDirectory(const std::string& serverId, const std::string& itemId);

    struct Impl;

private:
    DownloadManager();
    ~DownloadManager();
    std::unique_ptr<Impl> impl_;
};

// RAII subscription for screens: unsubscribes when destroyed (main thread).
//
//   sub_ = downloads::Subscription([this](const downloads::Event& e) { refresh(e); });
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(DownloadManager::Listener listener)
        : id_(DownloadManager::instance().subscribe(std::move(listener))) {}
    ~Subscription() { reset(); }
    Subscription(Subscription&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            reset();
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void reset() {
        if (id_) DownloadManager::instance().unsubscribe(id_);
        id_ = 0;
    }
    bool active() const { return id_ != 0; }

private:
    int id_ = 0;
};

}  // namespace karafin::downloads
