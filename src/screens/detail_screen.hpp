#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "screens/common.hpp"
#include "screens/widgets/action_button.hpp"
#include "screens/widgets/chips.hpp"
#include "screens/widgets/download_button.hpp"
#include "screens/widgets/overview_text.hpp"
#include "screens/widgets/row_stack.hpp"
#include "screens/widgets/skeleton.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/layout.hpp"
#include "ui/screen.hpp"
#include "ui/scroll.hpp"
#include "ui/widgets/backdrop.hpp"
#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::screens {

// Movie / episode / video page: full-bleed backdrop, logo or title, metadata and media badges,
// overview, Play (asks resume vs start over) / From start, audio + subtitle + version pickers,
// favorite, watched, download with live state, then more episodes, cast and similar rows.
class DetailScreen : public ui::Screen {
public:
    explicit DetailScreen(std::string itemId);
    ~DetailScreen() override;

    void onShow() override;
    void update(float dt) override;

private:
    enum Order : int { kEpisodes = 10, kCast = 20, kSimilar = 30 };
    static constexpr float kLateRefreshDelay = 2.0f;

    void load();
    void populate(const jellyfin::BaseItem& item, bool refresh);
    void updateUserState();
    void refreshUserState();
    void loadRows();
    void loadEpisodes(bool quiet);
    void openAudio();
    void openSubtitles();
    void openVersions();
    void toggleFavorite();
    void toggleWatched();
    void checkLogo();
    const jellyfin::MediaSource* source() const;
    PlayOptions playOptions() const;

    std::string itemId_;
    jellyfin::BaseItem item_;
    bool loaded_ = false;
    bool shownOnce_ = false;
    bool favPending_ = false;
    bool watchedPending_ = false;

    ui::Backdrop* backdrop_ = nullptr;
    ui::ScrollView* page_ = nullptr;
    ui::Box* header_ = nullptr;
    ui::ImageView* logo_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Label* subtitle_ = nullptr;
    ChipRow* chips_ = nullptr;
    ChipRow* media_ = nullptr;
    ui::Label* genres_ = nullptr;
    OverviewText* overview_ = nullptr;
    ui::Label* credits_ = nullptr;
    ui::Box* buttons_ = nullptr;
    ActionButton* play_ = nullptr;
    ActionButton* restart_ = nullptr;
    ActionButton* audio_ = nullptr;
    ActionButton* subs_ = nullptr;
    ActionButton* version_ = nullptr;
    ActionButton* series_ = nullptr;
    ActionButton* fav_ = nullptr;
    ActionButton* watched_ = nullptr;
    DownloadButton* download_ = nullptr;
    ui::Box* progressRow_ = nullptr;
    ui::ProgressBar* progress_ = nullptr;
    ui::Label* progressLabel_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    StatusView* status_ = nullptr;
    std::unique_ptr<RowStack> rows_;

    gfx::ImagePtr logoImage_;
    bool logoShown_ = false;

    size_t sourceIndex_ = 0;
    std::optional<int> audioIndex_;
    std::optional<int> subtitleIndex_;

    ui::Timeline::Handle lateRefresh_ = 0;
    CancelToken token_;
    CancelToken actionToken_;
};

}  // namespace karafin::screens
