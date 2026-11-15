#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "screens/widgets/action_button.hpp"
#include "screens/widgets/chips.hpp"
#include "screens/widgets/download_button.hpp"
#include "screens/widgets/skeleton.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/anim.hpp"
#include "ui/layout.hpp"
#include "ui/screen.hpp"
#include "ui/scroll.hpp"
#include "ui/widgets/backdrop.hpp"
#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/tab_bar.hpp"

namespace karafin::screens {

class EpisodeRow;

// Series page: backdrop, logo/title, metadata, overview, "Play S2:E4" (next up), download
// series, favorite, watched; seasons tab bar; per-season actions (download season, mark
// watched) and the episode list with per-episode download buttons; similar shows.
class SeriesScreen : public ui::Screen {
public:
    explicit SeriesScreen(std::string seriesId, std::string seasonId = {});
    ~SeriesScreen() override;

    void onShow() override;
    void update(float dt) override;

private:
    static constexpr float kLateRefreshDelay = 2.0f;

    void load();
    void loadSeasons();
    void populate(const jellyfin::BaseItem& series);
    void onSeasonsLoaded();
    void buildSeasonSelector();
    void selectSeason(size_t index, bool immediate);
    void loadEpisodes(size_t index);
    void showEpisodes(const std::vector<jellyfin::BaseItem>& episodes);
    void showEpisodesError(size_t index, const std::string& message);
    bool episodesHaveFocus() const;
    void clearEpisodes();
    void updateSeasonBar();
    void updatePlayButton();
    void updateUserButtons();
    void loadSimilar();
    void refreshQuietly();
    void toggleFavorite();
    void toggleWatched();
    void toggleSeasonWatched();
    void checkLogo();
    void openSeasonPicker();
    size_t episodeInsertIndex() const;

    std::string seriesId_;
    std::string preferredSeason_;
    jellyfin::BaseItem series_;
    std::vector<jellyfin::BaseItem> seasons_;
    std::map<std::string, std::vector<jellyfin::BaseItem>> cache_;
    std::optional<jellyfin::BaseItem> nextUp_;
    size_t selected_ = 0;
    bool loaded_ = false;
    bool seasonsReady_ = false;
    bool nextUpReady_ = false;
    bool shownOnce_ = false;

    ui::Backdrop* backdrop_ = nullptr;
    ui::ScrollView* page_ = nullptr;
    ui::Box* header_ = nullptr;
    ui::ImageView* logo_ = nullptr;
    ui::Label* title_ = nullptr;
    ChipRow* chips_ = nullptr;
    ui::Label* genres_ = nullptr;
    ui::Label* overview_ = nullptr;
    ActionButton* play_ = nullptr;
    DownloadButton* download_ = nullptr;
    ActionButton* fav_ = nullptr;
    ActionButton* watched_ = nullptr;
    ui::Box* tabsRow_ = nullptr;
    ui::TabBar* tabs_ = nullptr;
    ui::Button* seasonPicker_ = nullptr;
    ui::Box* seasonBar_ = nullptr;
    ui::Label* seasonLabel_ = nullptr;
    ActionButton* seasonWatched_ = nullptr;
    DownloadButton* seasonDownload_ = nullptr;
    ActionButton* seasonRetry_ = nullptr;
    std::string seasonsError_;
    std::vector<ui::View*> episodeRows_;
    ui::View* similar_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    StatusView* status_ = nullptr;

    gfx::ImagePtr logoImage_;
    bool logoShown_ = false;

    ui::Timeline::Handle seasonTimer_ = 0;
    ui::Timeline::Handle lateRefresh_ = 0;
    CancelToken token_;
    CancelToken seasonToken_;
    CancelToken actionToken_;
};

}  // namespace karafin::screens
