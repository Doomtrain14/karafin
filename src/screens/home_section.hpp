#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/error.hpp"
#include "jellyfin/models.hpp"
#include "screens/main_screen.hpp"
#include "screens/widgets/hero.hpp"
#include "screens/widgets/row_stack.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/anim.hpp"
#include "ui/scroll.hpp"
#include "ui/widgets/backdrop.hpp"
#include "ui/widgets/poster_card.hpp"
#include "ui/widgets/row_carousel.hpp"

namespace karafin::screens {

// Home: featured hero carousel over a crossfading backdrop, then Continue Watching, Next Up,
// Recently Added per library and My Libraries. Rows show shimmer skeletons while loading;
// Continue Watching / Next Up refresh quietly whenever Home is shown again. A failed hero or
// Recently Added row turns into an inline error with "Try again".
class HomeSection : public SectionView {
public:
    HomeSection();
    ~HomeSection() override;

    void onSectionShown(bool first) override;
    void onSectionHidden() override;
    float headerScrim() const override;
    bool onSectionBack() override;
    void onFocusWithin(ui::View* descendant) override;
    void update(float dt) override;

private:
    enum Order : int { kHeroError = 1, kResume = 10, kNextUp = 20, kLatest = 100, kLibraries = 1000 };
    static constexpr float kLateRefreshDelay = 2.0f;

    struct LatestRow {
        int order;
        std::string libraryId;
        std::string title;
        ui::CardShape shape;
    };

    void load();
    void refreshContinue();
    void loadViews();
    void loadHero();
    void loadResume(bool quiet);
    void loadNextUp(bool quiet);
    void loadLatest(const LatestRow& row, bool quiet);
    // Hides the hero and shows an inline error row (with Try again) in its place.
    void showHeroError(const jellyfin::ApiError& error);
    // Shows (or updates) an inline error row with Try again for a failed Recently Added row.
    void showLatestError(const LatestRow& row, const jellyfin::ApiError& error);
    void finishRequest(const jellyfin::ApiError* error);
    void checkEmpty();

    // Removes the row when empty, updates its cards in place when the items are the same, else
    // rebuilds it.
    void applyRow(int order, const std::string& title, const std::vector<jellyfin::BaseItem>& items,
                  ui::CardShape shape);
    std::unique_ptr<ui::RowCarousel> makeRow(const std::string& title, const std::vector<jellyfin::BaseItem>& items,
                                             ui::CardShape shape);
    static std::string signature(const std::vector<jellyfin::BaseItem>& items);
    void tryClaimFocus();
    void showBackdropFor(const jellyfin::BaseItem& item, float delay);

    ui::Backdrop* backdrop_ = nullptr;
    ui::ScrollView* page_ = nullptr;
    HeroCarousel* hero_ = nullptr;
    StatusView* status_ = nullptr;
    std::unique_ptr<RowStack> rows_;
    std::map<int, std::string> signatures_;
    std::vector<LatestRow> latestRows_;
    ui::Timeline::Handle lateRefresh_ = 0;

    CancelToken token_;
    CancelToken refreshToken_;
    int pending_ = 0;
    int failures_ = 0;
    jellyfin::ApiError lastError_;
    bool focusClaimed_ = false;
    bool heroDone_ = false;
    bool shown_ = false;

    jellyfin::BaseItem backdropItem_;
    float backdropDelay_ = -1;
    std::string backdropShownId_;
};

}  // namespace karafin::screens
