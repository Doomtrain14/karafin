#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jellyfin/playback.hpp"

namespace karafin::jellyfin {

class Client;

// Persistent queue of playback reports that could not be delivered (offline playback of
// downloads, or a Stopped report sent while the server was unreachable). Stored in
// sdmc:/switch/karafin/jellyfin_pending_reports.json and flushed automatically when a Client for
// the same server+user validates its session or regains connectivity. Thread-safe.
class ReportQueue {
public:
    static ReportQueue& instance();

    // Final position of a playback session. Replaces older queued Stopped reports for the same
    // item (only the latest position matters).
    void enqueueStopped(const std::string& serverId, const std::string& userId, const PlaybackReport& report);
    // Mark played / unplayed (e.g. an offline episode watched to the end).
    void enqueuePlayed(const std::string& serverId, const std::string& userId, const std::string& itemId,
                       bool played);

    // Pending entries, optionally filtered by server and user.
    size_t pending(const std::string& serverId = {}, const std::string& userId = {});

    // Sends the client's pending entries in order on the main thread's callback chain; stops at
    // the first connectivity/auth error (entries stay queued), drops entries the server rejects
    // (e.g. 404 for a deleted item). `done(sent, remaining)` runs on the main thread.
    using FlushCallback = std::function<void(int sent, int remaining)>;
    void flush(std::shared_ptr<Client> client, FlushCallback done = {});

    std::string filePath() const;

private:
    struct Entry {
        uint64_t id = 0;
        std::string serverId;
        std::string userId;
        std::string kind;       // "stopped", "played", "unplayed"
        std::string itemId;
        PlaybackReport report;  // kind == "stopped"
        int64_t createdAt = 0;
    };

    struct Snapshot {
        std::string text;
        uint64_t seq = 0;
    };

    ReportQueue() = default;
    void ensureLoaded();           // mutex held
    Snapshot serialize();          // mutex held
    void write(const Snapshot& s);  // mutex NOT held (SD I/O); drops snapshots older than the last write
    void remove(uint64_t id);
    void flushNext(std::shared_ptr<Client> client, std::shared_ptr<std::vector<Entry>> entries, size_t i, int sent,
                   FlushCallback done);

    std::mutex mutex_;
    bool loaded_ = false;
    uint64_t nextId_ = 1;
    uint64_t snapshotSeq_ = 0;
    std::vector<Entry> entries_;
    std::atomic<bool> flushing_{false};
    std::mutex writeMutex_;
    uint64_t writtenSeq_ = 0;
};

}  // namespace karafin::jellyfin
