#include "downloads/detail/queue.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "downloads/detail/util.hpp"

namespace karafin::downloads::detail {

namespace {

constexpr int kQueueVersion = 1;

Json subtitleToJson(const LocalSubtitle& s) {
    return Json{{"index", s.index},         {"file", s.fileName},    {"lang", s.language},
                {"title", s.title},         {"format", s.format},    {"default", s.isDefault},
                {"forced", s.isForced},     {"sdh", s.isHearingImpaired}};
}

LocalSubtitle subtitleFromJson(const Json& j) {
    LocalSubtitle s;
    s.index = static_cast<int>(jint(j, "index", -1));
    s.fileName = jstr(j, "file");
    s.language = jstr(j, "lang");
    s.title = jstr(j, "title");
    s.format = jstr(j, "format");
    s.isDefault = jbool(j, "default");
    s.isForced = jbool(j, "forced");
    s.isHearingImpaired = jbool(j, "sdh");
    return s;
}

}  // namespace

Json taskToJson(const Task& t) {
    const DownloadInfo& i = t.info;
    Json subs = Json::array();
    for (const auto& s : t.subtitles) subs.push_back(subtitleToJson(s));
    Json j = Json{
        {"serverId", i.serverId},
        {"userId", i.userId},
        {"itemId", i.itemId},
        {"mediaSourceId", i.mediaSourceId},
        {"type", i.type},
        {"name", i.name},
        {"seriesId", i.seriesId},
        {"seriesName", i.seriesName},
        {"seasonId", i.seasonId},
        {"seasonName", i.seasonName},
        {"index", i.indexNumber},
        {"parentIndex", i.parentIndexNumber},
        {"year", i.productionYear},
        {"runTimeTicks", i.runTimeTicks},
        {"container", i.container},
        {"overview", i.overview},
        {"state", std::string(stateName(i.state))},
        {"error", i.error},
        {"pausedByUser", i.pausedByUser},
        {"bytesDone", i.bytesDone},
        {"bytesTotal", i.bytesTotal},
        {"fileName", i.fileName},
        {"addedAt", i.addedAt},
        {"completedAt", i.completedAt},
        {"positionTicks", i.positionTicks},
        {"played", i.played},
        {"eTag", t.eTag},
        {"lastModified", t.lastModified},
        {"assetsDone", t.assetsDone},
        {"hasPoster", t.hasPoster},
        {"hasBackdrop", t.hasBackdrop},
        {"hasSeriesPoster", t.hasSeriesPoster},
        {"subtitles", std::move(subs)},
        {"seq", t.seq},
    };
    if (t.syncedBytes != kSyncedUnknown) j["syncedBytes"] = t.syncedBytes;
    return j;
}

uint64_t resumeOffset(uint64_t partFileSize, uint64_t syncedBytes) {
    return syncedBytes == kSyncedUnknown ? partFileSize : std::min(partFileSize, syncedBytes);
}

std::optional<Task> taskFromJson(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    Task t;
    DownloadInfo& i = t.info;
    i.serverId = jstr(j, "serverId");
    i.itemId = jstr(j, "itemId");
    if (i.serverId.empty() || i.itemId.empty()) return std::nullopt;
    i.key = makeKey(i.serverId, i.itemId);
    i.userId = jstr(j, "userId");
    i.mediaSourceId = jstr(j, "mediaSourceId");
    i.type = jstr(j, "type");
    i.name = jstr(j, "name");
    i.seriesId = jstr(j, "seriesId");
    i.seriesName = jstr(j, "seriesName");
    i.seasonId = jstr(j, "seasonId");
    i.seasonName = jstr(j, "seasonName");
    i.indexNumber = static_cast<int>(jint(j, "index", -1));
    i.parentIndexNumber = static_cast<int>(jint(j, "parentIndex", -1));
    i.productionYear = static_cast<int>(jint(j, "year", 0));
    i.runTimeTicks = jint(j, "runTimeTicks", 0);
    i.container = jstr(j, "container");
    i.overview = jstr(j, "overview");
    i.state = parseState(jstr(j, "state")).value_or(State::Paused);
    i.error = jstr(j, "error");
    i.pausedByUser = jbool(j, "pausedByUser");
    i.bytesDone = juint(j, "bytesDone");
    i.bytesTotal = juint(j, "bytesTotal");
    i.fileName = jstr(j, "fileName");
    i.addedAt = jint(j, "addedAt");
    i.completedAt = jint(j, "completedAt");
    i.positionTicks = jint(j, "positionTicks");
    i.played = jbool(j, "played");
    t.eTag = jstr(j, "eTag");
    t.lastModified = jstr(j, "lastModified");
    t.syncedBytes = juint(j, "syncedBytes", kSyncedUnknown);
    t.assetsDone = jbool(j, "assetsDone");
    t.hasPoster = jbool(j, "hasPoster");
    t.hasBackdrop = jbool(j, "hasBackdrop");
    t.hasSeriesPoster = jbool(j, "hasSeriesPoster");
    if (auto it = j.find("subtitles"); it != j.end() && it->is_array())
        for (const auto& s : *it)
            if (s.is_object()) t.subtitles.push_back(subtitleFromJson(s));
    t.seq = juint(j, "seq");
    return t;
}

void restoreTask(Task& t, bool autoResume) {
    DownloadInfo& i = t.info;
    t.running = false;
    i.bytesPerSecond = 0;
    i.etaSeconds = -1;
    i.attempt = 0;
    i.retryInSeconds = 0;
    switch (i.state) {
        case State::Queued:
        case State::Probing:
        case State::Downloading:
        case State::WaitingForNetwork:
            i.state = autoResume ? State::Queued : State::Paused;
            i.pausedByUser = false;
            i.error.clear();
            break;
        case State::Paused:
            if (autoResume && !i.pausedByUser) i.state = State::Queued;
            break;
        case State::Completed:
        case State::Failed: break;
    }
}

Task* Queue::find(std::string_view key) {
    for (auto& t : tasks_)
        if (t.info.key == key) return &t;
    return nullptr;
}

const Task* Queue::find(std::string_view key) const { return const_cast<Queue*>(this)->find(key); }

Task* Queue::findItem(std::string_view itemId) {
    for (auto& t : tasks_)
        if (t.info.itemId == itemId) return &t;
    return nullptr;
}

const Task* Queue::findItem(std::string_view itemId) const { return const_cast<Queue*>(this)->findItem(itemId); }

bool Queue::add(Task task) {
    task.info.key = makeKey(task.info.serverId, task.info.itemId);
    if (find(task.info.key)) return false;
    task.seq = nextSeq_++;
    tasks_.push_back(std::move(task));
    return true;
}

bool Queue::remove(std::string_view key) {
    const auto before = tasks_.size();
    std::erase_if(tasks_, [&](const Task& t) { return t.info.key == key; });
    return tasks_.size() != before;
}

bool Queue::transition(Task& t, State to, std::string error) {
    DownloadInfo& i = t.info;
    if (!canTransition(i.state, to)) {
        log::warn("downloads", "invalid transition {} -> {} for {}", stateName(i.state), stateName(to), i.key);
        return false;
    }
    if (i.state == State::Downloading && to != State::Downloading) {
        i.bytesPerSecond = 0;
        i.etaSeconds = -1;
    }
    i.state = to;
    if (to == State::Failed || to == State::WaitingForNetwork) {
        i.error = std::move(error);
    } else {
        i.error.clear();
    }
    if (to != State::WaitingForNetwork) i.retryInSeconds = 0;
    if (to != State::Paused) i.pausedByUser = false;
    return true;
}

bool Queue::pause(Task& t) {
    const State s = t.info.state;
    if (s != State::Queued && !isActive(s)) return false;
    if (!transition(t, State::Paused)) return false;
    t.info.pausedByUser = true;
    t.info.attempt = 0;
    return true;
}

bool Queue::resume(Task& t) {
    if (t.info.state != State::Paused) return false;
    return transition(t, State::Queued);
}

bool Queue::retry(Task& t) {
    if (t.info.state != State::Failed) return false;
    t.info.attempt = 0;
    return transition(t, State::Queued);
}

Task* Queue::nextRunnable(const std::function<bool(const Task&)>& skip) {
    Task* best = nullptr;
    for (auto& t : tasks_) {
        if (t.running || t.info.state != State::Queued) continue;
        if (skip && skip(t)) continue;
        if (!best || t.seq < best->seq) best = &t;
    }
    return best;
}

int Queue::runningCount() const {
    return static_cast<int>(std::count_if(tasks_.begin(), tasks_.end(), [](const Task& t) { return t.running; }));
}

Json Queue::toJson() const {
    Json items = Json::array();
    for (const auto& t : tasks_) items.push_back(taskToJson(t));
    return Json{{"version", kQueueVersion}, {"items", std::move(items)}};
}

bool Queue::fromJson(const Json& j, bool autoResume) {
    tasks_.clear();
    nextSeq_ = 1;
    if (!j.is_object()) return false;
    auto it = j.find("items");
    if (it == j.end() || !it->is_array()) return false;
    for (const auto& e : *it) {
        auto t = taskFromJson(e);
        if (!t || find(t->info.key)) continue;
        restoreTask(*t, autoResume);
        tasks_.push_back(std::move(*t));
    }
    std::stable_sort(tasks_.begin(), tasks_.end(), [](const Task& a, const Task& b) { return a.seq < b.seq; });
    // Renumber so stored gaps / zeros cannot collide with new additions.
    for (auto& t : tasks_) t.seq = nextSeq_++;
    return true;
}

}  // namespace karafin::downloads::detail
