#include "screens/widgets/row_stack.hpp"

#include <algorithm>

#include "ui/root.hpp"
#include "ui/widgets/row_carousel.hpp"

namespace karafin::screens {

using namespace karafin::ui;

View* RowStack::place(int order, std::unique_ptr<View> row) {
    size_t focusIdx = SIZE_MAX;
    size_t index = SIZE_MAX;
    bool fade = true;
    bool hadFocus = false;
    if (auto it = rows_.find(order); it != rows_.end()) {
        View* old = it->second;
        auto* oldRow = dynamic_cast<RowCarousel*>(old);
        // Skeleton -> content fades in; content -> content swaps without blinking.
        fade = oldRow == nullptr;
        hadFocus = old->hasFocusWithin();
        if (oldRow && hadFocus) focusIdx = oldRow->focusedIndex();
        index = box_->indexOf(old);
        box_->removeChild(old);
        rows_.erase(it);
    }
    if (index == SIZE_MAX) {
        index = std::min(leading_, box_->childCount());
        for (const auto& [o, v] : rows_)
            if (o < order) index = std::max(index, box_->indexOf(v) + 1);
    }
    View* raw = box_->insertChild(index, std::move(row));
    rows_[order] = raw;
    if (fade) {
        raw->alpha.set(0.0f);
        raw->alpha.tweenTo(1.0f, theme::motion::slow, ease::outCubic);
    }
    bool handed = false;
    if (focusIdx != SIZE_MAX)
        if (auto* rc = dynamic_cast<RowCarousel*>(raw); rc && rc->itemCount() > 0) {
            focus().setFocus(rc->itemAt(std::min(focusIdx, rc->itemCount() - 1)));
            handed = true;
        }
    if (hadFocus && !handed) {
        // E.g. an ErrorRow being retried: hand focus to the new row (or the nearest one).
        if (View* target = raw->defaultFocus()) focus().setFocus(target);
        else focusNear(order, 0);
    }
    return raw;
}

void RowStack::remove(int order) {
    auto it = rows_.find(order);
    if (it == rows_.end()) return;
    View* old = it->second;
    const bool hadFocus = old->hasFocusWithin();
    size_t focusIdx = 0;
    if (hadFocus)
        if (auto* rc = dynamic_cast<RowCarousel*>(old)) focusIdx = rc->focusedIndex();
    rows_.erase(it);
    box_->removeChild(old);
    if (hadFocus) focusNear(order, focusIdx);
}

void RowStack::focusNear(int order, size_t index) {
    auto usable = [](View* v) {
        auto* rc = dynamic_cast<RowCarousel*>(v);
        return rc && rc->itemCount() > 0 ? rc : nullptr;
    };
    RowCarousel* target = nullptr;
    for (auto it = rows_.upper_bound(order); it != rows_.end() && !target; ++it) target = usable(it->second);
    for (auto it = rows_.lower_bound(order); it != rows_.begin() && !target;) target = usable((--it)->second);
    if (target) focus().setFocus(target->itemAt(std::min(index, target->itemCount() - 1)));
}

void RowStack::clear() {
    for (const auto& [order, view] : rows_) box_->removeChild(view);
    rows_.clear();
}

View* RowStack::get(int order) const {
    auto it = rows_.find(order);
    return it == rows_.end() ? nullptr : it->second;
}

}  // namespace karafin::screens
