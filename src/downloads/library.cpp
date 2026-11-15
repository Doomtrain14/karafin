// Offline library (completed downloads grouped for browsing) and local playback progress.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/thread_pool.hpp"
#include "downloads/detail/manager_impl.hpp"
#include "downloads/detail/util.hpp"
#include "jellyfin/report_queue.hpp"

namespace karafin::downloads {

using detail::StoredMeta;
using detail::Task;

namespace {

constexpr const char* kTag = "downloads";
constexpr double kReportSettleSeconds = 3.0;

std::string lowerName(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Unknown numbers (-1) sort last.
int orderIndex(int n) { return n < 0 ? 1'000'000 : n; }

std::string seasonLabel(const OfflineItem& ep) {
    if (!ep.info.seasonName.empty()) return ep.info.seasonName;
    if (!ep.item.seasonName.empty()) return ep.item.seasonName;
    if (ep.info.parentIndexNumber == 0) return "Specials";
    if (ep.info.parentIndexNumber > 0) return "Season " + std::to_string(ep.info.parentIndexNumber);
    return "Episodes";
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Impl helpers

std::shared_ptr<const StoredMeta> DownloadManager::Impl::loadMeta(const Task& t) const {
    {
        std::lock_guard lock(metaMutex);
        if (auto it = metaCache.find(t.info.key); it != metaCache.end()) return it->second;
    }
    auto meta = std::make_shared<StoredMeta>();
    const std::string path = detail::itemDir(t.info.serverId, t.info.itemId) + "/" + detail::kMetadataFile;
    if (auto text = fs::readFileAtomic(path)) {
        const auto doc = nlohmann::json::parse(*text, nullptr, false);
        if (doc.is_object()) {
            if (auto it = doc.find("item"); it != doc.end() && it->is_object()) {
                meta->item = jellyfin::parseItem(*it);
                meta->hasItem = true;
            }
            if (auto it = doc.find("series"); it != doc.end() && it->is_object()) {
                meta->series = jellyfin::parseItem(*it);
                meta->hasSeries = true;
            }
            if (auto it = doc.find("season"); it != doc.end() && it->is_object()) {
                meta->season = jellyfin::parseItem(*it);
                meta->hasSeason = true;
            }
        }
    }
    if (!meta->hasItem) log::warn(kTag, "{}: metadata.json missing or invalid", t.info.key);
    std::lock_guard lock(metaMutex);
    metaCache[t.info.key] = meta;
    return meta;
}

void DownloadManager::Impl::invalidateMeta(const std::string& key) const {
    std::lock_guard lock(metaMutex);
    metaCache.erase(key);
}

OfflineItem DownloadManager::Impl::makeOfflineItem(const Task& t) const {
    OfflineItem o;
    o.info = t.info;
    o.dir = detail::itemDir(t.info.serverId, t.info.itemId);
    o.mediaPath = o.dir + "/" + t.info.fileName;
    if (t.hasPoster) o.posterPath = o.dir + "/" + detail::kPosterFile;
    if (t.hasBackdrop) o.backdropPath = o.dir + "/" + detail::kBackdropFile;
    if (t.hasSeriesPoster) o.seriesPosterPath = o.dir + "/" + detail::kSeriesPosterFile;
    for (LocalSubtitle s : t.subtitles) {
        s.path = o.dir + "/" + detail::kSubtitleDir + "/" + s.fileName;
        o.subtitles.push_back(std::move(s));
    }

    auto meta = loadMeta(t);
    if (meta->hasItem) {
        o.item = meta->item;
    } else {
        jellyfin::BaseItem& b = o.item;
        b.id = t.info.itemId;
        b.serverId = t.info.serverId;
        b.name = t.info.name;
        b.type = t.info.type;
        b.mediaType = "Video";
        b.seriesId = t.info.seriesId;
        b.seriesName = t.info.seriesName;
        b.seasonId = t.info.seasonId;
        b.seasonName = t.info.seasonName;
        b.indexNumber = t.info.indexNumber;
        b.parentIndexNumber = t.info.parentIndexNumber;
        b.productionYear = t.info.productionYear;
        b.runTimeTicks = t.info.runTimeTicks;
        b.container = t.info.container;
    }
    jellyfin::UserData& ud = o.item.userData;
    ud.playbackPositionTicks = t.info.positionTicks;
    ud.played = t.info.played;
    const int64_t runtime = o.item.runTimeTicks > 0 ? o.item.runTimeTicks : t.info.runTimeTicks;
    ud.playedPercentage = t.info.played ? 100.0
                          : runtime > 0 ? 100.0 * static_cast<double>(t.info.positionTicks) / static_cast<double>(runtime)
                                        : 0.0;
    return o;
}

void DownloadManager::Impl::patchMetadataFile(const std::string& serverId, const std::string& itemId,
                                              int64_t positionTicks, bool played) {
    const std::string path = detail::itemDir(serverId, itemId) + "/" + detail::kMetadataFile;
    auto text = fs::readFileAtomic(path);
    if (!text) return;
    auto doc = nlohmann::json::parse(*text, nullptr, false);
    if (!doc.is_object()) return;
    detail::patchUserData(doc, positionTicks, played);
    if (!fs::writeFileAtomic(path, detail::dumpJson(doc))) log::warn(kTag, "cannot update {}", path);
    invalidateMeta(makeKey(serverId, itemId));
}

void DownloadManager::Impl::queueReports(const Task& t, int64_t positionTicks, bool reportPlayed, bool played) {
    const DownloadInfo& i = t.info;
    if (i.userId.empty()) return;
    auto& rq = jellyfin::ReportQueue::instance();
    if (positionTicks >= 0) {
        jellyfin::PlaybackReport r;
        r.itemId = i.itemId;
        r.mediaSourceId = i.mediaSourceId;
        r.positionTicks = positionTicks;
        r.playMethod = jellyfin::PlayMethod::DirectPlay;
        rq.enqueueStopped(i.serverId, i.userId, r);
    }
    if (reportPlayed) rq.enqueuePlayed(i.serverId, i.userId, i.itemId, played);

    // Deliver when that server is reachable (otherwise the queue flushes on the next successful
    // session validation). Coalesce bursts of saves into one flush so a caller that queues on
    // every progress tick doesn't send a Stopped report per tick.
    static std::atomic<uint64_t> generation{0};
    const uint64_t gen = ++generation;
    main_thread::postDelayed(reportPlayed ? 1.0 : kReportSettleSeconds, [gen, serverId = i.serverId, userId = i.userId] {
        if (generation.load() != gen) return;
        auto c = jellyfin::currentClient();
        if (c && c->isAuthenticated() && !c->isOffline() && c->serverId() == serverId && c->userId() == userId)
            jellyfin::ReportQueue::instance().flush(c);
    });
}

// ------------------------------------------------------------------------------------------------
// Public API

OfflineLibrary DownloadManager::library(const std::string& serverId) const {
    std::vector<Task> tasks;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->ensureLoaded();
        for (const auto& t : impl_->queue.tasks()) {
            if (t.info.state != State::Completed) continue;
            if (!serverId.empty() && t.info.serverId != serverId) continue;
            tasks.push_back(t);
        }
    }

    OfflineLibrary lib;
    std::map<std::string, size_t> seriesIndex;  // serverId:seriesId -> lib.series index
    for (const auto& t : tasks) {
        OfflineItem o = impl_->makeOfflineItem(t);
        lib.totalBytes += o.info.bytesDone;
        if (!o.info.isEpisode() || o.info.seriesId.empty()) {
            lib.movies.push_back(std::move(o));
            continue;
        }

        const std::string skey = makeKey(o.info.serverId, o.info.seriesId);
        auto [it, inserted] = seriesIndex.emplace(skey, lib.series.size());
        if (inserted) {
            OfflineSeries s;
            s.serverId = o.info.serverId;
            s.id = o.info.seriesId;
            s.name = o.info.seriesName;
            auto meta = impl_->loadMeta(t);
            if (meta->hasSeries) {
                s.item = meta->series;
                if (s.name.empty()) s.name = s.item.name;
            } else {
                s.item.id = s.id;
                s.item.serverId = s.serverId;
                s.item.name = s.name;
                s.item.type = "Series";
            }
            lib.series.push_back(std::move(s));
        }
        OfflineSeries& series = lib.series[it->second];
        if (series.posterPath.empty()) series.posterPath = o.seriesPosterPath;
        if (series.backdropPath.empty()) series.backdropPath = o.backdropPath;
        series.bytes += o.info.bytesDone;
        ++series.episodeCount;

        const std::string seasonKey =
            !o.info.seasonId.empty() ? o.info.seasonId : "#" + std::to_string(o.info.parentIndexNumber);
        auto season = std::find_if(series.seasons.begin(), series.seasons.end(), [&](const OfflineSeason& s) {
            return (!s.id.empty() ? s.id : "#" + std::to_string(s.indexNumber)) == seasonKey;
        });
        if (season == series.seasons.end()) {
            OfflineSeason s;
            s.id = o.info.seasonId;
            s.indexNumber = o.info.parentIndexNumber;
            s.name = seasonLabel(o);
            auto meta = impl_->loadMeta(t);
            if (meta->hasSeason) s.item = meta->season;
            series.seasons.push_back(std::move(s));
            season = series.seasons.end() - 1;
        }
        season->bytes += o.info.bytesDone;
        season->episodes.push_back(std::move(o));
    }

    auto byName = [](const std::string& a, const std::string& b) { return lowerName(a) < lowerName(b); };
    std::stable_sort(lib.movies.begin(), lib.movies.end(),
                     [&](const OfflineItem& a, const OfflineItem& b) { return byName(a.info.name, b.info.name); });
    std::stable_sort(lib.series.begin(), lib.series.end(),
                     [&](const OfflineSeries& a, const OfflineSeries& b) { return byName(a.name, b.name); });
    for (auto& s : lib.series) {
        std::stable_sort(s.seasons.begin(), s.seasons.end(), [](const OfflineSeason& a, const OfflineSeason& b) {
            return orderIndex(a.indexNumber) < orderIndex(b.indexNumber);
        });
        for (auto& season : s.seasons) {
            std::stable_sort(season.episodes.begin(), season.episodes.end(), [](const OfflineItem& a, const OfflineItem& b) {
                return orderIndex(a.info.indexNumber) < orderIndex(b.info.indexNumber);
            });
        }
    }
    return lib;
}

void DownloadManager::loadLibrary(std::function<void(OfflineLibrary)> done, const std::string& serverId) const {
    workers().post([this, done = std::move(done), serverId] {
        auto lib = std::make_shared<OfflineLibrary>(library(serverId));
        main_thread::post([done, lib] {
            if (done) done(std::move(*lib));
        });
    });
}

std::optional<OfflineItem> DownloadManager::offlineItem(const std::string& key) const {
    Task copy;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->ensureLoaded();
        const Task* t = impl_->queue.find(key);
        if (!t || t->info.state != State::Completed) return std::nullopt;
        copy = *t;
    }
    return impl_->makeOfflineItem(copy);
}

std::optional<OfflineItem> DownloadManager::nextOfflineEpisode(const std::string& key) const {
    std::vector<Task> eps;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->ensureLoaded();
        const Task* cur = impl_->queue.find(key);
        if (!cur || !cur->info.isEpisode() || cur->info.seriesId.empty()) return std::nullopt;
        for (const auto& t : impl_->queue.tasks()) {
            if (t.info.state == State::Completed && t.info.isEpisode() && t.info.serverId == cur->info.serverId &&
                t.info.seriesId == cur->info.seriesId)
                eps.push_back(t);
        }
    }
    std::stable_sort(eps.begin(), eps.end(), [](const Task& a, const Task& b) {
        const int sa = orderIndex(a.info.parentIndexNumber), sb = orderIndex(b.info.parentIndexNumber);
        if (sa != sb) return sa < sb;
        return orderIndex(a.info.indexNumber) < orderIndex(b.info.indexNumber);
    });
    for (size_t i = 0; i + 1 < eps.size(); ++i)
        if (eps[i].info.key == key) return impl_->makeOfflineItem(eps[i + 1]);
    return std::nullopt;
}

void DownloadManager::saveLocalProgress(const std::string& key, int64_t positionTicks, bool finished,
                                        bool queueServerReport) {
    Impl& m = *impl_;
    Task copy;
    bool reached = false;
    bool newlyPlayed = false;
    bool ioRunning = false;
    {
        std::lock_guard lock(m.mutex);
        m.startLocked();
        ioRunning = m.started;
        Task* t = m.queue.find(key);
        if (!t) return;
        DownloadInfo& i = t->info;
        positionTicks = std::max<int64_t>(0, positionTicks);
        reached = finished || detail::countsAsPlayed(positionTicks, i.runTimeTicks);
        newlyPlayed = reached && !i.played;
        i.played = i.played || reached;
        i.positionTicks = reached ? 0 : positionTicks;
        copy = *t;
        m.emit(EventType::Updated, *t);
        m.markDirty(true);
    }
    // SD writes (metadata.json, the report queue) stay off the calling (main) thread. After
    // shutdown() the I/O thread is gone (player teardown runs after onStop): write inline.
    const int64_t reportTicks = reached && copy.info.runTimeTicks > 0 ? copy.info.runTimeTicks : positionTicks;
    auto work = [&m, copy, reportTicks, newlyPlayed, queueServerReport] {
        m.patchMetadataFile(copy.info.serverId, copy.info.itemId, copy.info.positionTicks, copy.info.played);
        if (queueServerReport) m.queueReports(copy, reportTicks, newlyPlayed, true);
    };
    if (ioRunning) {
        m.postIo(std::move(work));
    } else {
        work();
    }
}

void DownloadManager::setLocalPlayed(const std::string& key, bool played, bool queueServerReport) {
    Impl& m = *impl_;
    Task copy;
    bool ioRunning = false;
    {
        std::lock_guard lock(m.mutex);
        m.startLocked();
        ioRunning = m.started;
        Task* t = m.queue.find(key);
        if (!t) return;
        t->info.played = played;
        t->info.positionTicks = 0;
        copy = *t;
        m.emit(EventType::Updated, *t);
        m.markDirty(true);
    }
    auto work = [&m, copy, played, queueServerReport] {
        m.patchMetadataFile(copy.info.serverId, copy.info.itemId, 0, played);
        if (queueServerReport) m.queueReports(copy, -1, true, played);
    };
    if (ioRunning) {
        m.postIo(std::move(work));
    } else {
        work();
    }
}

}  // namespace karafin::downloads
