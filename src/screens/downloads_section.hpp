#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "downloads/manager.hpp"
#include "screens/main_screen.hpp"
#include "screens/widgets/action_button.hpp"
#include "screens/widgets/status_view.hpp"
#include "screens/widgets/storage_bar.hpp"
#include "ui/anim.hpp"
#include "ui/scroll.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::screens {

class PendingDownloadRow;

// Downloads: storage usage, overall speed and time left, "In progress" (progress, speed, ETA,
// size; pause / resume / cancel each, pause all / resume all, keep screen on) and "Downloaded" (movies, and shows grouped by series; play
// offline, mark watched, delete, delete watched). Live via DownloadManager events.
class DownloadsSection : public SectionView {
public:
    explicit DownloadsSection(MainMode mode);

    void onSectionShown(bool first) override;
    void onSectionHidden() override;
    float headerScrim() const override { return 1.0f; }
    ui::View* navigateFocus(ui::View* from, ui::NavDir dir) override;

private:
    ui::View* rowTarget(ui::View* row, const ui::View* from) const;
    void onEvent(const downloads::Event& e);
    void scheduleRebuild(float delay = 0.2f);
    void rebuild();
    void build(const downloads::OfflineLibrary& library);
    void updateHeader();
    void updateThroughput();
    void updateScreenOnButton();
    std::string focusedKey() const;

    MainMode mode_;
    ui::Label* title_ = nullptr;
    ui::Label* summary_ = nullptr;
    ActionButton* keepScreenOn_ = nullptr;
    ActionButton* pauseAll_ = nullptr;
    ActionButton* resumeAll_ = nullptr;
    ActionButton* deleteWatched_ = nullptr;
    StorageBar* storage_ = nullptr;
    ui::ScrollView* page_ = nullptr;
    StatusView* status_ = nullptr;
    std::map<std::string, PendingDownloadRow*> pendingRows_;
    std::map<const ui::View*, std::string> rowKeys_;
    std::vector<ui::View*> rows_;  // focusable rows, top to bottom
    downloads::ThroughputMeter meter_;
    std::chrono::steady_clock::time_point meterAt_{};
    std::string summaryLead_;  // counts before the speed / time left
    std::string summaryTail_;  // after it
    downloads::Subscription sub_;
    ui::Timeline::Handle rebuildTimer_ = 0;
    uint64_t generation_ = 0;
    bool visibleNow_ = false;
    bool dirty_ = true;
};

}  // namespace karafin::screens
