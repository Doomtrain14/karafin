#include "player/offline.hpp"

#include "core/log.hpp"

#if __has_include("downloads/manager.hpp")
#include "downloads/manager.hpp"
#define KARAFIN_HAS_DOWNLOADS 1
#else
#define KARAFIN_HAS_DOWNLOADS 0
#endif

namespace karafin::player::offline {

#if KARAFIN_HAS_DOWNLOADS

namespace {

Media convert(const downloads::OfflineItem& o) {
    Media m;
    m.key = o.info.key;
    m.mediaPath = o.mediaPath;
    m.item = o.item;
    m.positionTicks = o.positionTicks();
    for (const auto& s : o.subtitles) {
        LocalSubtitle l;
        l.index = s.index;
        l.path = s.path;
        l.language = s.language;
        l.title = s.title;
        l.format = s.format;
        l.isDefault = s.isDefault;
        l.isForced = s.isForced;
        l.isHearingImpaired = s.isHearingImpaired;
        m.subtitles.push_back(std::move(l));
    }
    return m;
}

}  // namespace

bool available() { return true; }

std::optional<Media> find(const std::string& itemId) {
    auto& dm = downloads::DownloadManager::instance();
    auto info = dm.findItem(itemId);
    if (!info || !info->isCompleted()) return std::nullopt;
    auto item = dm.offlineItem(info->key);
    if (!item) return std::nullopt;
    return convert(*item);
}

std::optional<Media> next(const std::string& key) {
    auto item = downloads::DownloadManager::instance().nextOfflineEpisode(key);
    if (!item) return std::nullopt;
    return convert(*item);
}

void saveProgress(const std::string& key, int64_t positionTicks, bool finished, bool queueReport) {
    downloads::DownloadManager::instance().saveLocalProgress(key, positionTicks, finished, queueReport);
}

void setStreamingThrottle(bool throttled) { downloads::DownloadManager::instance().setThrottled(throttled); }

#else

bool available() { return false; }
std::optional<Media> find(const std::string& itemId) {
    log::warn("player", "offline playback of {} requested but the downloads module is not built", itemId);
    return std::nullopt;
}
std::optional<Media> next(const std::string&) { return std::nullopt; }
void saveProgress(const std::string&, int64_t, bool, bool) {}
void setStreamingThrottle(bool) {}

#endif

}  // namespace karafin::player::offline
