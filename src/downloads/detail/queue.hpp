#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "downloads/types.hpp"

// The download queue as plain data: tasks, the state machine and (de)serialization. No locking,
// threads or I/O; DownloadManager wraps it with a mutex. Covered by downloads::selfTest.
namespace karafin::downloads::detail {

inline constexpr uint64_t kSyncedUnknown = UINT64_MAX;

struct Task {
    DownloadInfo info;
    std::string eTag;              // ETag of the server file the .part belongs to
    std::string lastModified;
    // Bytes of the .part known to be on the card (fsync'ed). After a crash or power loss the
    // file can be longer than this with an unwritten tail, so resuming starts here.
    // kSyncedUnknown: trust the file size.
    uint64_t syncedBytes = kSyncedUnknown;
    bool assetsDone = false;       // metadata.json, images and subtitles are bundled
    bool hasPoster = false;
    bool hasBackdrop = false;
    bool hasSeriesPoster = false;
    std::vector<LocalSubtitle> subtitles;  // fileName set, path empty (derived from the folder)
    uint64_t seq = 0;              // FIFO order

    // Runtime only (not persisted).
    bool running = false;
    std::chrono::steady_clock::time_point lastEmit{};
};

nlohmann::json taskToJson(const Task& t);
std::optional<Task> taskFromJson(const nlohmann::json& j);
// Where to resume a .part of `partFileSize` bytes: never past the last fsync'ed byte.
uint64_t resumeOffset(uint64_t partFileSize, uint64_t syncedBytes);
// App start: interrupted / queued work comes back Paused, or Queued with autoResume. Items the
// user paused stay paused. Clears runtime fields (speed, ETA, retry counters).
void restoreTask(Task& t, bool autoResume);

class Queue {
public:
    Task* find(std::string_view key);
    const Task* find(std::string_view key) const;
    // First task for this item id on any server.
    Task* findItem(std::string_view itemId);
    const Task* findItem(std::string_view itemId) const;

    // Appends with the next sequence number. False if the key already exists.
    bool add(Task task);
    bool remove(std::string_view key);
    void clear() { tasks_.clear(); }

    std::vector<Task>& tasks() { return tasks_; }
    const std::vector<Task>& tasks() const { return tasks_; }
    size_t size() const { return tasks_.size(); }

    // Moves `t` to `to` if canTransition() allows it (logs and returns false otherwise). Entering
    // Failed / WaitingForNetwork stores `error`; other states clear it. Leaving Downloading
    // zeroes the speed and ETA.
    static bool transition(Task& t, State to, std::string error = {});

    // User operations; true when the task changed.
    static bool pause(Task& t);   // Queued / Probing / Downloading / WaitingForNetwork -> Paused
    static bool resume(Task& t);  // Paused -> Queued
    static bool retry(Task& t);   // Failed -> Queued (clears the error and attempt counter)

    // Lowest-sequence Queued task that no job owns (and `skip` does not reject), or nullptr.
    Task* nextRunnable(const std::function<bool(const Task&)>& skip = {});
    int runningCount() const;

    // {"version":1,"items":[...]}
    nlohmann::json toJson() const;
    // Replaces the content from toJson() output and applies restoreTask(). Returns false (and
    // stays empty) when the document is not a queue.
    bool fromJson(const nlohmann::json& j, bool autoResume);

private:
    std::vector<Task> tasks_;
    uint64_t nextSeq_ = 1;
};

}  // namespace karafin::downloads::detail
