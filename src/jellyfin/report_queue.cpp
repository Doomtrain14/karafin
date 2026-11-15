#include "jellyfin/report_queue.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/paths.hpp"
#include "jellyfin/client.hpp"
#include "jellyfin/detail/json_get.hpp"

namespace karafin::jellyfin {

using Json = nlohmann::json;
using namespace detail;

namespace {
constexpr size_t kMaxEntries = 500;
}

ReportQueue& ReportQueue::instance() {
    static ReportQueue q;
    return q;
}

std::string ReportQueue::filePath() const { return paths::dataFile("jellyfin_pending_reports.json"); }

void ReportQueue::ensureLoaded() {
    if (loaded_) return;
    loaded_ = true;
    // Like fs::readFileAtomic(), but remembers which file was read so a damaged one can be moved.
    std::string source = filePath();
    auto data = fs::readFile(source);
    if (!data) {
        source += ".tmp";
        data = fs::readFile(source);
    }
    if (!data) return;
    const Json doc = Json::parse(*data, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        // Keep the damaged file as <name>.corrupt instead of overwriting it with the next report,
        // and so it is not parsed again on every start.
        const std::string target = filePath() + ".corrupt";
        if (fs::exists(target)) fs::removeFile(target);
        if (std::rename(source.c_str(), target.c_str()) == 0) {
            log::warn("jellyfin", "{} is corrupt; dropping pending reports (moved to {})", source, target);
        } else {
            log::error("jellyfin", "{} is corrupt; dropping pending reports (could not move it aside: {})", source,
                       std::strerror(errno));
        }
        return;
    }
    for (const auto& je : getArr(doc, "entries")) {
        Entry e;
        e.id = nextId_++;
        e.serverId = getStr(je, "serverId");
        e.userId = getStr(je, "userId");
        e.kind = getStr(je, "kind");
        e.itemId = getStr(je, "itemId");
        e.report = PlaybackReport::fromJson(getObj(je, "report"));
        e.createdAt = getI64(je, "createdAt");
        if (!e.serverId.empty() && !e.userId.empty() && !e.itemId.empty() && !e.kind.empty())
            entries_.push_back(std::move(e));
    }
    if (!entries_.empty()) log::info("jellyfin", "{} pending playback reports", entries_.size());
}

ReportQueue::Snapshot ReportQueue::serialize() {
    Json doc = {{"version", 1}, {"entries", Json::array()}};
    for (const auto& e : entries_) {
        Json je = {{"serverId", e.serverId}, {"userId", e.userId}, {"kind", e.kind},
                   {"itemId", e.itemId},     {"createdAt", e.createdAt}};
        if (e.kind == "stopped") je["report"] = e.report.toJson();
        doc["entries"].push_back(std::move(je));
    }
    return {dumpSafe(doc), ++snapshotSeq_};
}

void ReportQueue::write(const Snapshot& s) {
    std::lock_guard lock(writeMutex_);
    if (s.seq <= writtenSeq_) return;
    if (fs::writeFileAtomic(filePath(), s.text)) writtenSeq_ = s.seq;
    else log::error("jellyfin", "failed to write {}", filePath());
}

void ReportQueue::enqueueStopped(const std::string& serverId, const std::string& userId,
                                 const PlaybackReport& report) {
    if (serverId.empty() || userId.empty() || report.itemId.empty()) return;
    Snapshot snap;
    {
        std::lock_guard lock(mutex_);
        ensureLoaded();
        std::erase_if(entries_, [&](const Entry& e) {
            return e.kind == "stopped" && e.serverId == serverId && e.userId == userId && e.itemId == report.itemId;
        });
        Entry e;
        e.id = nextId_++;
        e.serverId = serverId;
        e.userId = userId;
        e.kind = "stopped";
        e.itemId = report.itemId;
        e.report = report;
        e.report.isPaused = false;
        e.report.eventName.clear();
        e.createdAt = static_cast<int64_t>(std::time(nullptr));
        entries_.push_back(std::move(e));
        if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin());
        snap = serialize();
    }
    write(snap);
}

void ReportQueue::enqueuePlayed(const std::string& serverId, const std::string& userId, const std::string& itemId,
                                bool played) {
    if (serverId.empty() || userId.empty() || itemId.empty()) return;
    Snapshot snap;
    {
        std::lock_guard lock(mutex_);
        ensureLoaded();
        std::erase_if(entries_, [&](const Entry& e) {
            return (e.kind == "played" || e.kind == "unplayed") && e.serverId == serverId && e.userId == userId &&
                   e.itemId == itemId;
        });
        Entry e;
        e.id = nextId_++;
        e.serverId = serverId;
        e.userId = userId;
        e.kind = played ? "played" : "unplayed";
        e.itemId = itemId;
        e.createdAt = static_cast<int64_t>(std::time(nullptr));
        entries_.push_back(std::move(e));
        if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin());
        snap = serialize();
    }
    write(snap);
}

size_t ReportQueue::pending(const std::string& serverId, const std::string& userId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return (serverId.empty() || e.serverId == serverId) && (userId.empty() || e.userId == userId);
    }));
}

void ReportQueue::remove(uint64_t id) {
    Snapshot snap;
    {
        std::lock_guard lock(mutex_);
        const size_t before = entries_.size();
        std::erase_if(entries_, [id](const Entry& e) { return e.id == id; });
        if (entries_.size() == before) return;
        snap = serialize();
    }
    write(snap);
}

void ReportQueue::flush(std::shared_ptr<Client> client, FlushCallback done) {
    auto finishEarly = [done](int remaining) {
        if (done) main_thread::post([done, remaining] { done(0, remaining); });
    };
    if (!client || !client->isAuthenticated()) return finishEarly(static_cast<int>(pending()));
    const std::string sid = client->serverId();
    const std::string uid = client->userId();
    auto batch = std::make_shared<std::vector<Entry>>();
    {
        std::lock_guard lock(mutex_);
        ensureLoaded();
        for (const auto& e : entries_)
            if (e.serverId == sid && e.userId == uid) batch->push_back(e);
    }
    if (batch->empty()) return finishEarly(0);
    if (flushing_.exchange(true)) return finishEarly(static_cast<int>(batch->size()));
    log::info("jellyfin", "flushing {} pending playback reports", batch->size());
    flushNext(std::move(client), std::move(batch), 0, 0, std::move(done));
}

// The chain runs on worker threads (each remove() rewrites the file); only `done` goes to the
// main thread.
void ReportQueue::flushNext(std::shared_ptr<Client> client, std::shared_ptr<std::vector<Entry>> entries, size_t i,
                            int sent, FlushCallback done) {
    auto finish = [this, client, done](int sentCount) {
        flushing_.store(false);
        const int remaining = static_cast<int>(pending(client->serverId(), client->userId()));
        if (done) main_thread::post([done, sentCount, remaining] { done(sentCount, remaining); });
    };
    if (i >= entries->size()) {
        log::info("jellyfin", "sent {} pending playback reports", sent);
        return finish(sent);
    }
    const Entry entry = (*entries)[i];
    Call<Unit> call;
    if (entry.kind == "stopped") {
        call = client->reportPlaybackStopped(entry.report, false);
    } else {
        call = client->setPlayed(entry.itemId, entry.kind == "played").map([](UserData) { return Unit{}; });
    }
    call.run(detail::Exec(false, {}), [this, client, entries, i, sent, done, entry, finish](Status s) {
        const bool keep = !s.ok() && (s.error.isConnectivity() || s.error.kind == ErrorKind::Unauthorized ||
                                      s.error.kind == ErrorKind::Cancelled || s.error.kind == ErrorKind::Server);
        if (keep) {
            log::info("jellyfin", "report flush paused: {}", s.error.describe());
            return finish(sent);
        }
        if (!s.ok()) log::warn("jellyfin", "dropping rejected report for {}: {}", entry.itemId, s.error.describe());
        remove(entry.id);
        flushNext(client, entries, i + 1, s.ok() ? sent + 1 : sent, done);
    });
}

}  // namespace karafin::jellyfin
