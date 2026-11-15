#include "ui/widgets/grid.hpp"

#include <algorithm>
#include <cmath>

#include "ui/root.hpp"

namespace karafin::ui {
namespace {
constexpr int kExtraRows = 1;  // rows kept alive above/below the viewport
}

Grid::Grid(Vec2 cellSize, float spacingX, float spacingY)
    : ScrollView(Axis::Vertical, std::make_unique<View>()),
      cellSize_(cellSize),
      spacingX_(spacingX),
      spacingY_(spacingY),
      padding_{theme::layout::marginX, theme::layout::focusOverscan, theme::layout::marginX, theme::layout::focusOverscan + 40} {
    setRememberFocus(true);
    setShowsIndicator(true);
    setRevealInsets(theme::layout::focusOverscan + 8, theme::layout::focusOverscan + 30);
}

void Grid::setCellSize(Vec2 size) {
    cellSize_ = size;
    reload();
}

void Grid::setPadding(const Insets& padding) {
    padding_ = padding;
    setNeedsLayout();
}

Rect Grid::cellRect(size_t index) const {
    const size_t col = index % columns_, row = index / columns_;
    return {originX_ + static_cast<float>(col) * (cellSize_.x + spacingX_),
            padding_.top + static_cast<float>(row) * (cellSize_.y + spacingY_), cellSize_.x, cellSize_.y};
}

void Grid::layoutContent() {
    const float avail = width() - padding_.horizontal();
    columns_ = static_cast<size_t>(std::max(1.0f, std::floor((avail + spacingX_) / (cellSize_.x + spacingX_))));
    const float used = static_cast<float>(columns_) * cellSize_.x + static_cast<float>(columns_ - 1) * spacingX_;
    originX_ = padding_.left + std::max(0.0f, (avail - used) * 0.5f);
    const size_t n = itemCount();
    const size_t rows = (n + columns_ - 1) / columns_;
    const float h = padding_.vertical() + (rows > 0 ? static_cast<float>(rows) * cellSize_.y +
                                                          static_cast<float>(rows - 1) * spacingY_
                                                    : 0.0f);
    setContentSize(h);
    contentView()->setFrame({0, 0, width(), std::max(h, height())});
    for (auto& s : active_) s.view->setFrame(cellRect(s.index));
    updateWindow();
}

void Grid::onScrollChanged() { updateWindow(); }

size_t Grid::indexOfCell(const View* v) const {
    for (const auto& s : active_)
        if (v && v->isDescendantOf(s.view)) return s.index;
    return SIZE_MAX;
}

View* Grid::cellAt(size_t index) const {
    for (const auto& s : active_)
        if (s.index == index) return s.view;
    return nullptr;
}

View* Grid::materialize(size_t index) {
    if (View* v = cellAt(index)) return v;
    if (index >= itemCount() || !createCell) return nullptr;
    View* v = nullptr;
    if (!pool_.empty()) {
        v = pool_.back();
        pool_.pop_back();
        v->setVisible(true);
    } else {
        v = contentView()->addChild(createCell());
    }
    v->setFrame(cellRect(index));
    if (bindCell) bindCell(*v, index);
    active_.push_back({v, index});
    return v;
}

void Grid::updateWindow() {
    const size_t n = itemCount();
    if (columns_ == 0 || height() <= 0) return;
    const float rowH = cellSize_.y + spacingY_;
    const float top = scrollOffset() - padding_.top;
    const int firstRow = std::max(0, static_cast<int>(std::floor(top / rowH)) - kExtraRows);
    const int lastRow = static_cast<int>(std::floor((top + height()) / rowH)) + kExtraRows;
    const size_t first = static_cast<size_t>(firstRow) * columns_;
    const size_t last = std::min(n, static_cast<size_t>(lastRow + 1) * columns_);

    View* focusedView = Root::exists() ? Root::instance().focus().focused() : nullptr;
    for (size_t i = 0; i < active_.size();) {
        Slot& s = active_[i];
        const bool keep = s.index < n && s.index >= first && s.index < last;
        const bool holdsFocus = focusedView && focusedView->isDescendantOf(s.view) && s.index < n;
        if (keep || holdsFocus) {
            ++i;
            continue;
        }
        s.view->setVisible(false);
        pool_.push_back(s.view);
        active_[i] = active_.back();
        active_.pop_back();
    }
    for (size_t idx = first; idx < last; ++idx) materialize(idx);

    if (onNearEnd && n > 0 && last + columns_ * 2 >= n && lastNearEnd_ != n) {
        lastNearEnd_ = n;
        onNearEnd();
    }
}

void Grid::reload() {
    for (auto& s : active_) {
        s.view->setVisible(false);
        pool_.push_back(s.view);
    }
    active_.clear();
    lastNearEnd_ = SIZE_MAX;
    if (focusedIndex_ >= itemCount()) focusedIndex_ = 0;
    setNeedsLayout();
}

void Grid::notifyAppended() { setNeedsLayout(); }

void Grid::focusIndex(size_t index) {
    if (index >= itemCount()) return;
    if (View* v = materialize(index)) Root::instance().focus().setFocus(v);
}

View* Grid::defaultFocus() {
    if (!visible() || itemCount() == 0) return nullptr;
    if (View* m = rememberedFocus(); m && m->isDescendantOf(this) && m->canFocus()) return m;
    View* v = materialize(std::min(focusedIndex_, itemCount() - 1));
    return v ? v->defaultFocus() : nullptr;
}

void Grid::onFocusWithin(View* descendant) {
    const size_t idx = indexOfCell(descendant);
    if (idx != SIZE_MAX) {
        focusedIndex_ = idx;
        if (onFocusIndex) onFocusIndex(idx);
    }
    ScrollView::onFocusWithin(descendant);
}

View* Grid::navigateFocus(View* from, NavDir dir) {
    const size_t idx = indexOfCell(from);
    const size_t n = itemCount();
    if (idx == SIZE_MAX || n == 0) return nullptr;
    const size_t col = idx % columns_;
    size_t target = SIZE_MAX;
    switch (dir) {
        case NavDir::Left:
            if (col == 0) return nullptr;
            target = idx - 1;
            break;
        case NavDir::Right:
            if (col + 1 >= columns_ || idx + 1 >= n) return View::focusBlocked();
            target = idx + 1;
            break;
        case NavDir::Up:
            if (idx < columns_) return nullptr;
            target = idx - columns_;
            break;
        case NavDir::Down: {
            const size_t row = idx / columns_, lastRow = (n - 1) / columns_;
            if (row >= lastRow) return View::focusBlocked();
            target = std::min(idx + columns_, n - 1);
            break;
        }
    }
    View* v = materialize(target);
    return v ? v : View::focusBlocked();
}

bool Grid::onInput(const InputEvent& e) {
    if ((e.button == Key::ZL || e.button == Key::ZR) && e.pressedOrRepeat()) {
        const size_t n = itemCount();
        if (n == 0) return true;
        const size_t rowsPerPage = std::max<size_t>(1, static_cast<size_t>(height() / (cellSize_.y + spacingY_)));
        const size_t jump = rowsPerPage * columns_;
        size_t target = focusedIndex_;
        if (e.button == Key::ZR) target = std::min(n - 1, focusedIndex_ + jump);
        else target = focusedIndex_ >= jump ? focusedIndex_ - jump : focusedIndex_ % columns_;
        focusIndex(target);
        return true;
    }
    return ScrollView::onInput(e);
}

}  // namespace karafin::ui
