#include "ui/widgets/row_carousel.hpp"

#include <cmath>
#include <limits>

namespace karafin::ui {
namespace {
// A focused poster grows ~9 px above its art (1.08x around the art center) plus the 3.5 px
// focus ring, so the cards start far enough below the title's line box to never touch it.
constexpr float kTitleGap = 16;

float titleHeight() { return std::ceil(gfx::text::lineHeight(theme::type::title3)); }
}  // namespace

RowCarousel::RowCarousel(std::string title) : inset_(theme::layout::marginX) {
    setRememberFocus(true);
    title_ = add<Label>(std::move(title), theme::type::title3, theme::color::textPrimary);
    scroll_ = add<ScrollView>(Axis::Horizontal);
    scroll_->setAnchor(ScrollAnchor::Leading);
    scroll_->setRevealInsets(inset_, inset_);
    scroll_->setPagingEnabled(false);
    Box* box = scroll_->content();
    box->setAlign(Align::Start);
    box->setSpacing(theme::layout::cardSpacing);
    const float o = theme::layout::focusOverscan;
    box->setPadding({inset_, o, inset_, o});
    setCullMargin(0);
}

void RowCarousel::setTitle(std::string title) {
    title_->setText(std::move(title));
    title_->setVisible(!title_->text().empty());
    setNeedsLayout();
}

void RowCarousel::setInset(float inset) {
    inset_ = inset;
    scroll_->setRevealInsets(inset, inset);
    const float o = theme::layout::focusOverscan;
    scroll_->content()->setPadding({inset, o, inset, o});
    setNeedsLayout();
}

float RowCarousel::headerHeight() { return titleHeight() + kTitleGap; }

Vec2 RowCarousel::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    const float titleH = title_->text().empty() ? 0.0f : headerHeight();
    const float rowH = scroll_->content()->measure({std::numeric_limits<float>::infinity(), available.y}).y;
    return {pref.x >= 0 ? pref.x : available.x, pref.y >= 0 ? pref.y : titleH + rowH - theme::layout::focusOverscan};
}

void RowCarousel::layout() {
    const float titleH = title_->text().empty() ? 0.0f : headerHeight();
    title_->setFrame({inset_, 0, width() - inset_ * 2, titleHeight()});
    // The scroller starts above its content padding so focused cards can grow into the gap.
    const float o = theme::layout::focusOverscan;
    const float rowH = scroll_->content()->measure({std::numeric_limits<float>::infinity(), height()}).y;
    scroll_->setFrame({0, titleH - o, width(), rowH});
}

size_t RowCarousel::indexOfItem(const View* v) const {
    Box* box = items();
    for (const View* p = v; p; p = p->parent())
        if (p->parent() == box) return box->indexOf(p);
    return SIZE_MAX;
}

size_t RowCarousel::focusedIndex() const {
    if (View* m = rememberedFocus()) return indexOfItem(m);
    return 0;
}

View* RowCarousel::navigateFocus(View* from, NavDir dir) {
    if (dir != NavDir::Left && dir != NavDir::Right) return nullptr;
    const size_t idx = indexOfItem(from);
    if (idx == SIZE_MAX) return nullptr;
    Box* box = items();
    const int step = dir == NavDir::Right ? 1 : -1;
    for (int i = static_cast<int>(idx) + step; i >= 0 && i < static_cast<int>(box->childCount()); i += step) {
        View* c = box->childAt(static_cast<size_t>(i));
        if (View* f = c->defaultFocus()) return f;
    }
    return blockEdges_ ? View::focusBlocked() : nullptr;
}

void RowCarousel::onFocusWithin(View* descendant) {
    if (!onNearEnd) return;
    const size_t idx = indexOfItem(descendant);
    const size_t n = itemCount();
    if (idx != SIZE_MAX && idx + nearEndThreshold >= n && n != lastNearEndCount_) {
        lastNearEndCount_ = n;
        onNearEnd();
    }
}

}  // namespace karafin::ui
