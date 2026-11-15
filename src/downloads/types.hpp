#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jellyfin/models.hpp"

// Public value types of the downloads module. Everything here is a plain copyable snapshot; the
// DownloadManager hands out copies, so holding one never blocks the download threads.
namespace karafin::downloads {

// Lifecycle of one download (a movie or a single episode).
//
//   Queued -> Probing -> Downloading -> Completed
//      ^         |            |
//      |         +------------+--> WaitingForNetwork (backoff) -> Probing ...
//      |         +------------+--> Failed (error)  -- retry() --> Queued
//      +-- resume() -- Paused <-- pause() (from Queued / Probing / Downloading / WaitingForNetwork)
enum class State { Queued, Probing, Downloading, Paused, WaitingForNetwork, Completed, Failed };

std::string_view stateName(State s);  // "Queued", "Probing", ...
std::optional<State> parseState(std::string_view name);
// Allowed edges of the state machine above (self-transitions are allowed for every state).
bool canTransition(State from, State to);
// Probing / Downloading / WaitingForNetwork: a job thread owns the item.
bool isActive(State s);
// Not Completed (Queued, Probing, Downloading, Paused, WaitingForNetwork, Failed).
inline bool isPending(State s) { return s != State::Completed; }

// "<serverId>:<itemId>" - identifies a download across servers.
std::string makeKey(std::string_view serverId, std::string_view itemId);

// One external subtitle file bundled next to the media file.
struct LocalSubtitle {
    int index = -1;            // Jellyfin MediaStream index (report as SubtitleStreamIndex)
    std::string fileName;      // "<index>.<lang>[.forced][.sdh].<fmt>" inside subtitles/
    std::string path;          // absolute (filled in snapshots / OfflineItem)
    std::string language;      // ISO 639-2, may be empty
    std::string title;         // DisplayTitle
    std::string format;        // "srt", "ass", "vtt", ...
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
};

// Snapshot of one download.
struct DownloadInfo {
    std::string key;           // makeKey(serverId, itemId)
    std::string serverId;
    std::string userId;        // account whose token downloads it and receives offline progress
    std::string itemId;
    std::string mediaSourceId;

    // Display data (copied from the BaseItem at enqueue time, refreshed from the metadata).
    std::string type;          // "Movie", "Episode", "Video", ...
    std::string name;
    std::string seriesId;
    std::string seriesName;
    std::string seasonId;
    std::string seasonName;
    int indexNumber = -1;        // episode number
    int parentIndexNumber = -1;  // season number
    int productionYear = 0;
    int64_t runTimeTicks = 0;
    std::string container;       // "mkv", "mp4", ...
    std::string overview;        // item summary (episode summary for episodes)

    State state = State::Queued;
    std::string error;           // Failed: user-facing reason. WaitingForNetwork: last error.
    bool pausedByUser = false;   // Paused by the user (vs. restored as paused on app start)

    uint64_t bytesDone = 0;      // bytes in <file>.part (or the final file when Completed)
    uint64_t bytesTotal = 0;     // 0 when unknown
    double bytesPerSecond = 0;   // smoothed; 0 unless Downloading
    double etaSeconds = -1;      // -1 when unknown
    int attempt = 0;             // consecutive failed attempts (WaitingForNetwork)
    double retryInSeconds = 0;   // WaitingForNetwork: seconds until the next attempt

    std::string fileName;        // media file name inside the item folder ("media.mkv")
    int64_t addedAt = 0;         // unix seconds
    int64_t completedAt = 0;

    // Local playback progress (offline watching).
    int64_t positionTicks = 0;
    bool played = false;

    bool isEpisode() const { return type == "Episode"; }
    bool isCompleted() const { return state == State::Completed; }
    // 0..1, or 0 when the total is unknown.
    double progress() const {
        if (state == State::Completed) return 1.0;
        return bytesTotal > 0 ? static_cast<double>(bytesDone) / static_cast<double>(bytesTotal) : 0.0;
    }
};

// Aggregate state of all known episodes below a series or season (detail screens).
struct GroupSummary {
    int total = 0;          // downloads known for this series/season
    int completed = 0;
    int active = 0;         // Probing / Downloading / WaitingForNetwork
    int queued = 0;
    int paused = 0;
    int failed = 0;
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;
    bool any() const { return total > 0; }
    bool allCompleted() const { return total > 0 && completed == total; }
    double progress() const {
        return bytesTotal > 0 ? static_cast<double>(bytesDone) / static_cast<double>(bytesTotal) : 0.0;
    }
};

struct StorageStats {
    uint64_t downloadedBytes = 0;  // completed media files
    uint64_t partialBytes = 0;     // .part files of unfinished downloads
    uint64_t reservedBytes = 0;    // still to download for unfinished items (known sizes only)
    uint64_t freeBytes = 0;        // SD card free space (0 if unknown)
    uint64_t totalBytes = 0;       // SD card size (0 if unknown)
    int completedCount = 0;
    int pendingCount = 0;
};

// Overall speed and time left of the queue (Downloads tab header). Feed it pending() snapshots
// with the time since the previous update. The summed speed is smoothed over about 3 s so it
// doesn't drop to zero while the next episode is being probed.
class ThroughputMeter {
public:
    void update(const std::vector<DownloadInfo>& downloads, double dtSeconds);
    bool active() const { return active_; }           // anything Probing / Downloading / WaitingForNetwork
    double bytesPerSecond() const { return speed_; }  // 0 unless something is Probing / Downloading
    // Bytes left of Queued and active downloads over the smoothed speed; -1 while idle, before
    // any speed is known or when one of them has no size yet.
    double etaSeconds() const { return eta_; }

private:
    bool active_ = false;
    double speed_ = 0;
    double eta_ = -1;
};

// --- Offline library (completed downloads) --------------------------------------------------

struct OfflineItem {
    DownloadInfo info;
    // Parsed from metadata.json; userData reflects the local playback progress.
    jellyfin::BaseItem item;
    std::string dir;               // item folder
    std::string mediaPath;         // file for mpv
    std::string posterPath;        // "" if not bundled
    std::string backdropPath;
    std::string seriesPosterPath;  // episodes only
    std::vector<LocalSubtitle> subtitles;  // absolute paths, for mpv `sub-files`

    int64_t positionTicks() const { return info.positionTicks; }
    bool played() const { return info.played; }
};

struct OfflineSeason {
    std::string id;
    std::string name;
    int indexNumber = -1;
    jellyfin::BaseItem item;            // from the season JSON (may be mostly empty)
    std::vector<OfflineItem> episodes;  // sorted by episode number
    uint64_t bytes = 0;
};

struct OfflineSeries {
    std::string serverId;
    std::string id;
    std::string name;
    jellyfin::BaseItem item;            // from the series JSON (may be mostly empty)
    std::string posterPath;             // series poster bundled with one of its episodes
    std::string backdropPath;
    std::vector<OfflineSeason> seasons; // sorted by season number
    int episodeCount = 0;
    uint64_t bytes = 0;
};

struct OfflineLibrary {
    std::vector<OfflineItem> movies;    // movies and other standalone videos, by name
    std::vector<OfflineSeries> series;  // by name
    uint64_t totalBytes = 0;
    bool empty() const { return movies.empty() && series.empty(); }
};

// --- Observer events ------------------------------------------------------------------------

enum class EventType {
    Added,     // new download in the list
    Updated,   // state, error or metadata changed
    Progress,  // bytes / speed / ETA (throttled to ~4 per second per item)
    Removed,   // cancelled or deleted (info holds the last snapshot)
};

// Bulk operations (pauseAll, removeWatched, ...) emit one event per affected item.
struct Event {
    EventType type = EventType::Updated;
    DownloadInfo info;
};

// Result of DownloadManager::enqueue (delivered on the main thread).
struct EnqueueResult {
    int added = 0;            // new downloads queued (or failed ones re-queued)
    int skipped = 0;          // already queued / downloaded, or not downloadable (virtual episodes)
    std::string error;        // user-facing message when nothing could be queued
    std::vector<std::string> keys;  // keys of the added downloads
    bool ok() const { return error.empty(); }
};

}  // namespace karafin::downloads
