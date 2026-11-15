#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "ui/scroll.hpp"

namespace karafin::ui {

// Virtualized vertical grid: only rows near the viewport have live cell views; cells are pooled
// and re-bound as you scroll. D-pad moves by index (row/column), ZL/ZR page, and onNearEnd fires
// when the visible window approaches the last loaded item (infinite scroll).
//
//   auto* grid = add<Grid>(PosterCard::artSize(CardShape::Poster) + Vec2{0, 46});
//   grid->count = [this] { return items_.size(); };
//   grid->createCell = [] { return std::make_unique<PosterCard>(); };
//   grid->bindCell = [this](View& v, size_t i) {
//       auto& card = static_cast<PosterCard&>(v);
//       card.setTitle(items_[i].name);
//       card.onActivate = [this, i] { open(i); };
//   };
//   grid->onNearEnd = [this] { fetchNextPage(); };   // then grid->notifyAppended()
//   grid->reload();
class Grid : public ScrollView {
public:
    explicit Grid(Vec2 cellSize, float spacingX = theme::layout::cardSpacing, float spacingY = 30);

    std::function<size_t()> count;
    std::function<std::unique_ptr<View>()> createCell;
    std::function<void(View& cell, size_t index)> bindCell;
    std::function<void()> onNearEnd;
    std::function<void(size_t index)> onFocusIndex;

    void setCellSize(Vec2 size);
    void setPadding(const Insets& padding);
    // Data replaced: rebinds every visible cell, keeps the scroll position when possible.
    void reload();
    // Items were appended: grows the content without rebinding existing cells.
    void notifyAppended();

    size_t columns() const { return columns_; }
    size_t focusedIndex() const { return focusedIndex_; }
    void focusIndex(size_t index);
    // Live cell for `index` (nullptr if not materialized).
    View* cellAt(size_t index) const;

    View* navigateFocus(View* from, NavDir dir) override;
    View* defaultFocus() override;
    void onFocusWithin(View* descendant) override;
    bool onInput(const InputEvent& e) override;

protected:
    void layoutContent() override;
    void onScrollChanged() override;

private:
    struct Slot {
        View* view;
        size_t index;
    };
    size_t itemCount() const { return count ? count() : 0; }
    Rect cellRect(size_t index) const;
    View* materialize(size_t index);
    void updateWindow();
    size_t indexOfCell(const View* v) const;

    Vec2 cellSize_;
    float spacingX_, spacingY_;
    Insets padding_;
    size_t columns_ = 1;
    float originX_ = 0;
    std::vector<Slot> active_;
    std::vector<View*> pool_;
    size_t focusedIndex_ = 0;
    size_t lastNearEnd_ = SIZE_MAX;
};

}  // namespace karafin::ui
