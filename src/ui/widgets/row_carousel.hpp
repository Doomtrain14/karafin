#pragma once

#include <string>

#include "ui/scroll.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::ui {

// Titled horizontal carousel (Apple TV style): the focused card aligns to the left content
// margin and the row glides with a spring; focus is remembered per row.
//
//   auto* row = page->content()->add<RowCarousel>("Continue Watching");
//   for (auto& item : items) row->items()->add<PosterCard>(CardShape::Wide)->setTitle(item.name);
//   row->onNearEnd = [=] { loadMore(); };
class RowCarousel : public View {
public:
    explicit RowCarousel(std::string title = {});

    void setTitle(std::string title);
    Label* titleLabel() const { return title_; }
    ScrollView* scroller() const { return scroll_; }
    Box* items() const { return scroll_->content(); }
    size_t itemCount() const { return items()->childCount(); }
    View* itemAt(size_t i) const { return items()->childAt(i); }
    void setItemSpacing(float s) { items()->setSpacing(s); }
    // Left/right content inset (defaults to the screen margin).
    void setInset(float inset);
    // Row ends stop focus instead of jumping spatially (possibly into another row's off-screen
    // cards). Disable when something sits beside the row (e.g. a sidebar). Default true.
    void setBlockEdges(bool v) { blockEdges_ = v; }

    // Fires when focus gets within `threshold` items of the end (lazy paging).
    std::function<void()> onNearEnd;
    size_t nearEndThreshold = 4;
    // Index of the focused (or remembered) item.
    size_t focusedIndex() const;
    // Title line plus the gap above the cards (where the cards start in a titled row).
    static float headerHeight();

    Vec2 measure(Vec2 available) override;
    void layout() override;
    View* navigateFocus(View* from, NavDir dir) override;
    void onFocusWithin(View* descendant) override;

private:
    size_t indexOfItem(const View* v) const;

    Label* title_ = nullptr;
    ScrollView* scroll_ = nullptr;
    float inset_;
    bool blockEdges_ = true;
    size_t lastNearEndCount_ = 0;
};

}  // namespace karafin::ui
