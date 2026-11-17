#include "screens/offline_series_screen.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <vector>

#include "screens/common.hpp"
#include "screens/widgets/download_rows.hpp"
#include "screens/main_screen.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace dl = karafin::downloads;

namespace {
constexpr float kPageY = 200;
}

OfflineSeriesScreen::OfflineSeriesScreen(std::string serverId, std::string seriesId)
    : serverId_(std::move(serverId)), seriesId_(std::move(seriesId)) {
    onAppMenu = [] { showAppMenu(); };
    poster_ = add<ImageView>();
    poster_->setFrame({theme::layout::marginX, 40, 96, 144});
    poster_->setRadius(theme::radius::md);
    poster_->setFallbackIcon(gfx::Icon::Tv);
    title_ = add<Label>("", theme::type::largeTitle);
    title_->setFrame({theme::layout::marginX + 120, 70, 760, 44});
    summary_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
    summary_->setFrame({theme::layout::marginX + 120, 120, 760, 22});

    auto* buttons = add<Box>(Axis::Horizontal);
    buttons->setSpacing(12);
    buttons->setFrame({theme::layout::screenWidth - theme::layout::marginX - 108, 76, 108, 48});
    deleteWatched_ = buttons->add<ActionButton>("", gfx::Icon::Check, ButtonStyle::Icon, "Delete watched");
    deleteAll_ = buttons->add<ActionButton>("", gfx::Icon::Trash, ButtonStyle::Icon, "Delete all");
    deleteAll_->onActivate = [this] {
        const std::string sid = serverId_, id = seriesId_;
        present(Dialog::confirm("Delete all episodes?", "Every downloaded episode of this show will be removed.",
                                "Delete all", "Keep", [sid, id](bool ok) {
                                    if (ok) dl::DownloadManager::instance().removeGroup(sid, id);
                                }));
    };

    page_ = add<ScrollView>(Axis::Vertical);
    page_->setFrame({0, kPageY, theme::layout::screenWidth, theme::layout::screenHeight - kPageY});
    page_->setRevealInsets(40, 64);
    page_->setShowsIndicator(true);
    page_->setClipsChildren(true);
    page_->content()->setSpacing(6);
    page_->content()->setPadding({0, 4, 0, 70});

    status_ = add<StatusView>();
    status_->setFrame({0, kPageY, theme::layout::screenWidth, theme::layout::screenHeight - kPageY - 60});

    sub_ = dl::Subscription(guard([this](const dl::Event& e) { onEvent(e); }));
    rebuild();
}

namespace {
ui::View* findFocusId(ui::View* root, const std::string& id) {
    if (!root || id.empty()) return nullptr;
    if (root->focusId() == id && root->canFocus()) return root;
    for (const auto& child : root->children())
        if (ui::View* found = findFocusId(child.get(), id)) return found;
    return nullptr;
}
}  // namespace

void OfflineSeriesScreen::onEvent(const dl::Event& e) {
    if (e.info.serverId != serverId_ || e.info.seriesId != seriesId_) return;
    auto it = rows_.find(e.info.key);
    const bool shown = it != rows_.end();
    // Bytes / speed / ETA. The completed episode list does not gain or lose rows here, so
    // refresh the existing row (or ignore it) instead of rebuilding the focused list.
    if (e.type == dl::EventType::Progress) {
        if (shown) {
            const dl::DownloadInfo& info = e.info;
            const std::string title =
                info.indexNumber >= 0 ? std::format("{}. {}", info.indexNumber, info.name) : info.name;
            const uint64_t bytes = info.bytesTotal > 0 ? info.bytesTotal : info.bytesDone;
            it->second->setText(title, joinDots({formatRuntime(info.runTimeTicks), formatBytes(bytes)}),
                                info.played ? "Watched" : "");
        }
        return;
    }
    if (e.type == dl::EventType::Added) return;
    if (e.type == dl::EventType::Updated && shown && e.info.isCompleted()) {
        bindRow(it->second, e.info);
        refreshChrome();
        return;
    }
    if (e.type == dl::EventType::Updated && !shown && !e.info.isCompleted()) return;
    if (e.type == dl::EventType::Removed && !shown) return;
    scheduleRebuild();
}

void OfflineSeriesScreen::scheduleRebuild() {
    if (timer_) return;
    timer_ = timeline().after(0.2f, [this] {
        timer_ = 0;
        rebuild();
    }, lifetime());
}

void OfflineSeriesScreen::bindRow(OfflineItemRow* row, const dl::DownloadInfo& info) {
    const std::string title = info.indexNumber >= 0 ? std::format("{}. {}", info.indexNumber, info.name) : info.name;
    const uint64_t bytes = info.bytesTotal > 0 ? info.bytesTotal : info.bytesDone;
    row->setText(title, joinDots({formatRuntime(info.runTimeTicks), formatBytes(bytes)}), info.played ? "Watched" : "");
    if (info.runTimeTicks > 0 && info.positionTicks > 0)
        row->setProgress(static_cast<float>(static_cast<double>(info.positionTicks) / static_cast<double>(info.runTimeTicks)));
    else
        row->setProgress(-1);
    row->setWatched(info.played);
    const std::string key = info.key, id = info.itemId, name = info.name;
    const int64_t pos = info.played ? 0 : info.positionTicks, runtime = info.runTimeTicks;
    const bool played = info.played;
    row->onActivate = [id, name, pos, runtime] { playOffline(id, name, pos, runtime); };
    row->onToggleWatched = [key, played] {
        dl::DownloadManager::instance().setLocalPlayed(key, !played);
        toast(played ? "Marked as unwatched" : "Marked as watched", ToastKind::Success, gfx::Icon::Check, 1.6f);
    };
    row->onDelete = [key, name] {
        present(Dialog::confirm("Delete episode?", "\"" + name + "\" will be removed from the SD card.", "Delete", "Keep",
                                [key](bool ok) {
                                    if (ok) dl::DownloadManager::instance().remove(key);
                                }));
    };
}

void OfflineSeriesScreen::refreshChrome() {
    int watched = 0;
    std::vector<std::string> watchedKeys;
    watchedKeys.reserve(rows_.size());
    for (const auto& [key, row] : rows_) {
        if (!row->watched()) continue;
        ++watched;
        watchedKeys.push_back(key);
    }
    summary_->setText(joinDots({std::format("{} episode{} downloaded", episodeCount_, episodeCount_ == 1 ? "" : "s"),
                                watched > 0 ? std::format("{} watched", watched) : "", formatBytes(seriesBytes_)}));
    deleteAll_->setEnabled(episodeCount_ > 0);
    deleteWatched_->setEnabled(!watchedKeys.empty());
    deleteWatched_->onActivate = [watchedKeys] {
        present(Dialog::confirm("Delete watched episodes?",
                                std::format("{} watched episode{} will be removed.", watchedKeys.size(),
                                            watchedKeys.size() == 1 ? "" : "s"),
                                "Delete", "Keep", [watchedKeys](bool ok) {
                                    if (!ok) return;
                                    for (const auto& k : watchedKeys) dl::DownloadManager::instance().remove(k);
                                }));
    };
}

void OfflineSeriesScreen::onShow() {
    if (shownOnce_) rebuild();  // local progress changes after offline playback
    shownOnce_ = true;
}

void OfflineSeriesScreen::rebuild() {
    const uint64_t gen = ++generation_;
    dl::DownloadManager::instance().loadLibrary(
        guard([this, gen](dl::OfflineLibrary lib) {
            if (gen == generation_) build(lib);
        }),
        serverId_);
}

void OfflineSeriesScreen::build(const dl::OfflineLibrary& library) {
    const dl::OfflineSeries* series = nullptr;
    for (const auto& s : library.series)
        if (s.id == seriesId_ && (s.serverId.empty() || s.serverId == serverId_)) series = &s;
    Box* box = page_->content();
    const bool hadFocus = page_->hasFocusWithin();
    const std::string keepFocusId = focus().focused() ? focus().focused()->focusId() : std::string();
    size_t focusIndex = 0;
    if (hadFocus)
        for (size_t i = 0; i < box->childCount(); ++i)
            if (box->childAt(i)->hasFocusWithin()) focusIndex = i;
    box->clearChildren();
    rows_.clear();
    if (!series || series->episodeCount == 0) {
        episodeCount_ = 0;
        seriesBytes_ = 0;
        page_->setVisible(false);
        deleteWatched_->setEnabled(false);
        deleteAll_->setEnabled(false);
        status_->showEmpty(gfx::Icon::Download, "No episodes left", "All downloads of this show were removed.",
                           {{"Back", gfx::Icon::Back, [] { nav().pop(); }, true}});
        status_->focusAction();
        return;
    }
    status_->hide();
    page_->setVisible(true);
    title_->setText(series->name);
    if (!series->posterPath.empty() && !poster_->image()) poster_->setUrl(series->posterPath);
    episodeCount_ = series->episodeCount;
    seriesBytes_ = series->bytes;

    for (const auto& season : series->seasons) {
        auto label = std::make_unique<Label>(
            joinDots({season.name.empty() ? std::format("Season {}", season.indexNumber) : season.name,
                      std::format("{} episode{}", season.episodes.size(), season.episodes.size() == 1 ? "" : "s"),
                      formatBytes(season.bytes)}),
            theme::type::overline, theme::color::accentBright);
        label->setMargin({theme::layout::marginX + 4, 14, theme::layout::marginX, 4});
        box->addChild(std::move(label));
        for (const auto& ep : season.episodes) {
            auto row = std::make_unique<OfflineItemRow>("", "", "", ep.posterPath, true);
            row->setFocusId(ep.info.key);
            OfflineItemRow* raw = row.get();
            raw->setSummary(ep.item.overview);
            bindRow(raw, ep.info);
            rows_[ep.info.key] = raw;
            box->addChild(std::move(row));
        }
    }
    refreshChrome();

    View* target = findFocusId(box, keepFocusId);
    if (!target && (hadFocus || !hasFocusWithin())) {
        for (size_t i = std::min(focusIndex, box->childCount() - 1); i < box->childCount() && !target; ++i)
            if (box->childAt(i)->canFocus()) target = box->childAt(i);
        for (size_t i = 0; i < box->childCount() && !target; ++i)
            if (box->childAt(i)->canFocus()) target = box->childAt(i);
    }
    if (target) focus().setFocus(target);
}

}  // namespace karafin::screens
