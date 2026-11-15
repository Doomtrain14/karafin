#include "screens/downloads_section.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <vector>

#include "screens/common.hpp"
#include "screens/offline_series_screen.hpp"
#include "screens/widgets/download_rows.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace dl = karafin::downloads;

namespace {
constexpr float kTitleY = 100;
constexpr float kStorageY = 184;  // below the header buttons' focus captions
constexpr float kPageY = 232;

std::unique_ptr<Label> sectionLabel(const std::string& text) {
    auto l = std::make_unique<Label>(text, theme::type::overline, theme::color::accentBright);
    l->setMargin({theme::layout::marginX + 4, 14, theme::layout::marginX, 4});
    return l;
}

View* findFocusId(View* root, const std::string& id) {
    if (!root || id.empty()) return nullptr;
    if (root->focusId() == id && root->canFocus()) return root;
    for (const auto& child : root->children())
        if (View* found = findFocusId(child.get(), id)) return found;
    return nullptr;
}

void snapPendingPlate(View* v) {
    for (View* p = v; p; p = p->parent()) {
        if (auto* row = dynamic_cast<PendingDownloadRow*>(p)) {
            row->snapFocusHighlight();
            return;
        }
    }
}
}  // namespace

DownloadsSection::DownloadsSection(MainMode mode) : mode_(mode) {
    title_ = add<Label>("Downloads", theme::type::title1);
    title_->setFrame({theme::layout::marginX, kTitleY, 500, 36});
    summary_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
    summary_->setFrame({theme::layout::marginX, kTitleY + 38, 700, 22});

    auto* buttons = add<Box>(Axis::Horizontal);
    buttons->setSpacing(12);
    const float bw = 3 * 48 + 2 * 12;
    buttons->setFrame({theme::layout::screenWidth - theme::layout::marginX - bw, kTitleY + 2, bw, 48});
    pauseAll_ = buttons->add<ActionButton>("", gfx::Icon::Pause, ButtonStyle::Icon, "Pause all");
    resumeAll_ = buttons->add<ActionButton>("", gfx::Icon::Play, ButtonStyle::Icon, "Resume all");
    deleteWatched_ = buttons->add<ActionButton>("", gfx::Icon::Trash, ButtonStyle::Icon, "Delete watched");
    pauseAll_->onActivate = [] {
        dl::DownloadManager::instance().pauseAll();
        toast("All downloads paused", ToastKind::Info, gfx::Icon::Pause, 1.8f);
    };
    resumeAll_->onActivate = [] {
        dl::DownloadManager::instance().resumeAll();
        dl::DownloadManager::instance().retryFailed();
        toast("Downloads resumed", ToastKind::Success, gfx::Icon::Download, 1.8f);
    };
    deleteWatched_->onActivate = [] {
        present(Dialog::confirm("Delete watched downloads?",
                                "Every downloaded movie and episode you have finished watching will be removed from "
                                "the SD card.",
                                "Delete watched", "Cancel", [](bool ok) {
                                    if (!ok) return;
                                    const int n = dl::DownloadManager::instance().removeWatched();
                                    toast(n > 0 ? std::format("Deleted {} watched download{}", n, n == 1 ? "" : "s")
                                                : "Nothing watched to delete",
                                          ToastKind::Info, gfx::Icon::Trash, 2.2f);
                                }));
    };

    storage_ = add<StorageBar>();
    storage_->setFrame({theme::layout::marginX, kStorageY, theme::layout::screenWidth - 2 * theme::layout::marginX, 40});

    page_ = add<ScrollView>(Axis::Vertical);
    page_->setFrame({0, kPageY, theme::layout::screenWidth, theme::layout::screenHeight - kPageY});
    page_->setRevealInsets(40, 64);
    page_->setShowsIndicator(true);
    page_->setClipsChildren(true);
    page_->content()->setSpacing(6);
    page_->content()->setPadding({0, 4, 0, 70});
    page_->content()->setRememberFocus(true);

    status_ = add<StatusView>();
    status_->setFrame({0, kPageY, theme::layout::screenWidth, theme::layout::screenHeight - kPageY - 60});

    sub_ = dl::Subscription(guard([this](const dl::Event& e) { onEvent(e); }));
}

void DownloadsSection::onSectionShown(bool first) {
    visibleNow_ = true;
    if (first || dirty_) rebuild();
    else updateHeader();
}

void DownloadsSection::onSectionHidden() { visibleNow_ = false; }

void DownloadsSection::onEvent(const dl::Event& e) {
    if (e.type == dl::EventType::Progress || e.type == dl::EventType::Updated) {
        auto it = pendingRows_.find(e.info.key);
        if (it != pendingRows_.end() && e.info.state != dl::State::Completed) {
            it->second->setInfo(e.info);
            if (e.type == dl::EventType::Updated) updateHeader();
            return;
        }
        if (e.type == dl::EventType::Progress) return;
    }
    scheduleRebuild();
}

void DownloadsSection::scheduleRebuild(float delay) {
    dirty_ = true;
    if (!visibleNow_ || rebuildTimer_) return;
    rebuildTimer_ = timeline().after(delay, [this] {
        rebuildTimer_ = 0;
        rebuild();
    }, lifetime());
}

void DownloadsSection::rebuild() {
    dirty_ = false;
    const uint64_t gen = ++generation_;
    if (page_->content()->childCount() == 0 && !status_->showing()) status_->showLoading();
    dl::DownloadManager::instance().loadLibrary(guard([this, gen](dl::OfflineLibrary lib) {
        if (gen != generation_) return;
        build(lib);
    }));
}

std::string DownloadsSection::focusedKey() const {
    const View* f = focus().focused();
    for (const View* v = f; v; v = v->parent()) {
        if (auto it = rowKeys_.find(v); it != rowKeys_.end()) return it->second;
    }
    return {};
}

void DownloadsSection::build(const dl::OfflineLibrary& library) {
    auto& dm = dl::DownloadManager::instance();
    const std::string keepKey = focusedKey();
    const std::string keepFocusId = focus().focused() ? focus().focused()->focusId() : std::string();
    const bool hadFocus = page_->hasFocusWithin();
    Box* box = page_->content();
    // Position of the focused row among the rows, so focus lands on its neighbour when that
    // download was just removed.
    size_t keepOrdinal = SIZE_MAX;
    {
        size_t ordinal = 0;
        for (size_t i = 0; i < box->childCount(); ++i) {
            const View* child = box->childAt(i);
            if (!rowKeys_.count(child)) continue;
            if (child->hasFocusWithin()) keepOrdinal = ordinal;
            ++ordinal;
        }
    }
    box->clearChildren();
    pendingRows_.clear();
    rowKeys_.clear();
    rows_.clear();
    View* restore = nullptr;
    std::vector<View*> rows;
    auto track = [&](View* v, const std::string& key) {
        v->setFocusId(key);
        rowKeys_[v] = key;
        rows.push_back(v);
        if (!keepKey.empty() && key == keepKey) restore = v;
    };

    const auto pending = dm.pending();
    if (!pending.empty()) {
        box->addChild(sectionLabel(std::format("IN PROGRESS ({})", pending.size())));
        for (const auto& info : pending) {
            auto* row = static_cast<PendingDownloadRow*>(box->addChild(std::make_unique<PendingDownloadRow>(info)));
            pendingRows_[info.key] = row;
            track(row, info.key);
        }
    }

    if (!library.movies.empty()) {
        box->addChild(sectionLabel(std::format("MOVIES ({})", library.movies.size())));
        for (const auto& m : library.movies) {
            const dl::DownloadInfo& info = m.info;
            const uint64_t bytes = info.bytesTotal > 0 ? info.bytesTotal : info.bytesDone;
            std::string sub = joinDots({info.productionYear > 0 ? std::to_string(info.productionYear) : "",
                                        formatRuntime(info.runTimeTicks), formatBytes(bytes)});
            const std::string detail = info.played ? "Watched" : (info.positionTicks > 0 ? "In progress" : "");
            auto row = std::make_unique<OfflineItemRow>(info.name, sub, detail, m.posterPath);
            if (info.runTimeTicks > 0 && info.positionTicks > 0)
                row->setProgress(static_cast<float>(static_cast<double>(info.positionTicks) / static_cast<double>(info.runTimeTicks)));
            row->setWatched(info.played);
            row->setSummary(m.item.overview);
            const std::string key = info.key, id = info.itemId, name = info.name;
            const int64_t pos = info.played ? 0 : info.positionTicks, runtime = info.runTimeTicks;
            const bool played = info.played;
            row->onActivate = [id, name, pos, runtime] { playOffline(id, name, pos, runtime); };
            row->onToggleWatched = [key, played] {
                dl::DownloadManager::instance().setLocalPlayed(key, !played);
                toast(played ? "Marked as unwatched" : "Marked as watched", ToastKind::Success, gfx::Icon::Check, 1.6f);
            };
            row->onDelete = [key, name, bytes] {
                present(Dialog::confirm("Delete download?", "\"" + name + "\" (" + formatBytes(bytes) + ") will be removed.",
                                        "Delete", "Keep", [key](bool ok) {
                                            if (ok) dl::DownloadManager::instance().remove(key);
                                        }));
            };
            row->refreshHints();
            track(box->addChild(std::move(row)), "item:" + key);
        }
    }

    if (!library.series.empty()) {
        box->addChild(sectionLabel(std::format("SHOWS ({})", library.series.size())));
        for (const auto& s : library.series) {
            int watched = 0;
            for (const auto& season : s.seasons)
                for (const auto& ep : season.episodes)
                    if (ep.info.played) ++watched;
            std::string sub = joinDots({std::format("{} episode{}", s.episodeCount, s.episodeCount == 1 ? "" : "s"),
                                        s.seasons.size() > 1 ? std::format("{} seasons", s.seasons.size()) : "",
                                        formatBytes(s.bytes)});
            auto row = std::make_unique<OfflineItemRow>(s.name.empty() ? "Unknown series" : s.name, sub,
                                                        watched > 0 ? std::format("{} watched", watched) : "", s.posterPath);
            row->setChevron(true);
            row->setSummary(s.item.overview);
            row->setWatched(watched > 0 && watched == s.episodeCount);
            const std::string serverId = s.serverId, seriesId = s.id, name = s.name;
            row->onActivate = [serverId, seriesId] {
                nav().push(std::make_unique<OfflineSeriesScreen>(serverId, seriesId));
            };
            row->onDelete = [serverId, seriesId, name] {
                present(Dialog::confirm("Delete all episodes?", "Every downloaded episode of \"" + name + "\" will be removed.",
                                        "Delete", "Keep", [serverId, seriesId](bool ok) {
                                            if (ok) dl::DownloadManager::instance().removeGroup(serverId, seriesId);
                                        }));
            };
            row->refreshHints();
            track(box->addChild(std::move(row)), "series:" + serverId + ":" + seriesId);
        }
    }

    rows_ = rows;
    updateHeader();
    if (box->childCount() == 0) {
        page_->setVisible(false);
        status_->showEmpty(gfx::Icon::Download, "No downloads yet",
                           mode_ == MainMode::Offline
                               ? "Downloaded movies and episodes appear here and play without a connection."
                               : "Open a movie or episode and choose Download to watch it offline later.");
        return;
    }
    const bool firstShow = !page_->visible() || status_->showing();
    status_->hide();
    page_->setVisible(true);
    if (firstShow) {
        page_->alpha.set(0.0f);
        page_->alpha.tweenTo(1.0f, theme::motion::slow, ease::outCubic);
    }
    if (View* exact = findFocusId(box, keepFocusId)) {
        // Same download (or the same pause/cancel control) was already focused: keep the plate
        // and let onFocus snap instead of springing the highlight in from zero.
        snapPendingPlate(exact);
        focus().setFocus(exact);
    } else if (hadFocus && !rows.empty()) {
        View* target = restore;
        if (!target) target = rows[keepOrdinal == SIZE_MAX ? 0 : std::min(keepOrdinal, rows.size() - 1)];
        focus().setFocus(target);
    }
}

// Up/Down walk the rows in list order. A spatial search would let the header buttons (which line
// up with each in-progress row's pause/cancel buttons) win while the list is still scrolling.
View* DownloadsSection::navigateFocus(View* from, NavDir dir) {
    if ((dir != NavDir::Up && dir != NavDir::Down) || !from || !from->isDescendantOf(page_->content()))
        return nullptr;
    size_t index = rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (from == rows_[i] || from->isDescendantOf(rows_[i])) {
            index = i;
            break;
        }
    }
    if (index == rows_.size()) return nullptr;
    if (dir == NavDir::Up) return index == 0 ? nullptr : rowTarget(rows_[index - 1], from);
    return index + 1 < rows_.size() ? rowTarget(rows_[index + 1], from) : focusBlocked();
}

// Keeps the same control column (pause/resume vs cancel) when moving between in-progress rows.
View* DownloadsSection::rowTarget(View* row, const View* from) const {
    if (row->canFocus()) return row;
    const auto key = rowKeys_.find(row);
    if (key != rowKeys_.end()) {
        for (const char* prefix : {"download-toggle:", "download-cancel:"}) {
            if (!from->focusId().starts_with(prefix)) continue;
            if (View* v = findFocusId(row, prefix + key->second)) return v;
        }
    }
    return row;
}

void DownloadsSection::updateHeader() {
    auto& dm = dl::DownloadManager::instance();
    const dl::StorageStats st = dm.storage();
    storage_->setUsage(st.downloadedBytes + st.partialBytes, st.freeBytes, st.totalBytes);
    int active = 0, paused = 0, failed = 0;
    for (const auto& [key, row] : pendingRows_) {
        (void)row;
        auto info = dm.find(key);
        if (!info) continue;
        if (info->state == dl::State::Paused) ++paused;
        else if (info->state == dl::State::Failed) ++failed;
        else ++active;
    }
    summary_->setText(joinDots({active > 0 ? std::format("{} downloading", active) : "",
                                paused > 0 ? std::format("{} paused", paused) : "",
                                failed > 0 ? std::format("{} failed", failed) : "",
                                st.completedCount > 0 ? std::format("{} downloaded", st.completedCount) : "",
                                st.reservedBytes > 0 ? formatBytes(st.reservedBytes) + " to go" : ""}));
    pauseAll_->setEnabled(active > 0);
    resumeAll_->setEnabled(paused > 0 || failed > 0);
    deleteWatched_->setEnabled(st.completedCount > 0);
}

}  // namespace karafin::screens
