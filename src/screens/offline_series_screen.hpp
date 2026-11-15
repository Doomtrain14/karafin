#pragma once

#include <string>
#include <unordered_map>

#include "downloads/manager.hpp"
#include "screens/widgets/action_button.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/anim.hpp"
#include "ui/screen.hpp"
#include "ui/scroll.hpp"
#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::screens {

class OfflineItemRow;

// Downloaded episodes of one series, grouped by season: play offline, mark watched, delete,
// delete watched / delete all. Works without a connection.
class OfflineSeriesScreen : public ui::Screen {
public:
    OfflineSeriesScreen(std::string serverId, std::string seriesId);

    void onShow() override;

private:
    void onEvent(const downloads::Event& e);
    void scheduleRebuild();
    void rebuild();
    void build(const downloads::OfflineLibrary& library);
    void bindRow(OfflineItemRow* row, const downloads::DownloadInfo& info);
    void refreshChrome();

    std::string serverId_;
    std::string seriesId_;
    ui::ImageView* poster_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Label* summary_ = nullptr;
    ActionButton* deleteWatched_ = nullptr;
    ActionButton* deleteAll_ = nullptr;
    ui::ScrollView* page_ = nullptr;
    StatusView* status_ = nullptr;
    downloads::Subscription sub_;
    ui::Timeline::Handle timer_ = 0;
    uint64_t generation_ = 0;
    bool shownOnce_ = false;
    int episodeCount_ = 0;
    uint64_t seriesBytes_ = 0;
    // Download key -> row. Progress updates these in place; the views (and their focus
    // animation) are only rebuilt when the set of episodes changes.
    std::unordered_map<std::string, OfflineItemRow*> rows_;
};

}  // namespace karafin::screens
