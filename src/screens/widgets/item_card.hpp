#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "ui/widgets/poster_card.hpp"
#include "ui/widgets/row_carousel.hpp"

namespace karafin::screens {

// PosterCard bound to a Jellyfin item. A opens it, X plays it (resume prompt), Y toggles watched.
class ItemCard : public ui::PosterCard {
public:
    explicit ItemCard(ui::CardShape shape = ui::CardShape::Poster, bool showTitles = true);
    ~ItemCard() override;

    void setItem(const jellyfin::BaseItem& item);
    // Shimmer placeholder while the item's page is still loading (grids).
    void setLoading();
    bool isLoading() const { return loading_; }
    const jellyfin::BaseItem& item() const { return item_; }

    // Overrides the default "open item" action.
    std::function<void(const jellyfin::BaseItem&)> onOpen;
    // Gets button presses first; return true to consume them (replaces X / Y handling).
    std::function<bool(const ui::InputEvent&)> onKey;

    bool onInput(const ui::InputEvent& e) override;
    void update(float dt) override;
    void drawOverlay(ui::Renderer& r) override;

private:
    void toggleWatched();

    jellyfin::BaseItem item_;
    std::string imageUrl_;
    std::string watchedPendingId_;  // item id with a watched request in flight
    CancelToken token_;
    bool loading_ = false;
    float phase_ = 0;
};

// Rebinds the ItemCards of `row` when it shows exactly these items (same ids, same order), so a
// refresh updates progress / watched state without rebuilding the row (keeps focus and scroll).
// Returns false when the row has to be rebuilt.
bool updateCardsInPlace(ui::View* row, const std::vector<jellyfin::BaseItem>& items);

}  // namespace karafin::screens
