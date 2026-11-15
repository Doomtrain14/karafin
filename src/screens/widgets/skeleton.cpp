#include "screens/widgets/skeleton.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/text.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/row_carousel.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kPeriod = 1.4f;
}  // namespace

Skeleton& Skeleton::addRect(const Rect& r, float radius) {
    rects_.push_back({r, radius});
    if (parent()) parent()->setNeedsLayout();
    return *this;
}

Vec2 Skeleton::measure(Vec2 available) {
    (void)available;
    Rect b;
    for (const auto& s : rects_) b = b.united(s.rect);
    const Vec2 pref = preferredSize();
    return {pref.x >= 0 ? pref.x : b.right(), pref.y >= 0 ? pref.y : b.bottom()};
}

void Skeleton::update(float dt) {
    phase_ = std::fmod(phase_ + dt / kPeriod, 1.0f);
    View::update(dt);
}

void Skeleton::draw(Renderer& r) {
    for (const auto& s : rects_) r.fillShimmer(s.rect, s.radius, theme::color::skeleton, phase_);
}

SkeletonRow::SkeletonRow(CardShape shape, int count, bool title) : shape_(shape), count_(count), title_(title) {}

Vec2 SkeletonRow::measure(Vec2 available) {
    const Vec2 art = PosterCard::artSize(shape_);
    const float titles = 10 + gfx::text::lineHeight(theme::type::callout) + gfx::text::lineHeight(theme::type::footnote);
    // Same geometry as a RowCarousel of real cards, so swapping one for the other doesn't jump.
    return {available.x, (title_ ? RowCarousel::headerHeight() : 0.0f) + art.y + titles + theme::layout::focusOverscan};
}

void SkeletonRow::update(float dt) {
    phase_ = std::fmod(phase_ + dt / kPeriod, 1.0f);
    View::update(dt);
}

void SkeletonRow::draw(Renderer& r) {
    const float x0 = theme::layout::marginX;
    float y = 0;
    if (title_) {
        r.fillShimmer({x0, 3, 200, 18}, 6, theme::color::skeleton, phase_);
        y = RowCarousel::headerHeight();
    }
    const Vec2 art = PosterCard::artSize(shape_);
    for (int i = 0; i < count_; ++i) {
        const float x = x0 + static_cast<float>(i) * (art.x + theme::layout::cardSpacing);
        if (x > width()) break;
        // Stagger the phase slightly per card so the shimmer sweeps across the row.
        const float ph = std::fmod(phase_ + static_cast<float>(i) * 0.04f, 1.0f);
        r.fillShimmer({x, y, art.x, art.y}, theme::card::radius, theme::color::skeleton, ph);
        r.fillShimmer({x + 2, y + art.y + 12, art.x * 0.7f, 12}, 5, theme::color::skeleton, ph);
        r.fillShimmer({x + 2, y + art.y + 30, art.x * 0.4f, 10}, 5, theme::color::skeleton, ph);
    }
}

SkeletonGrid::SkeletonGrid(Vec2 cellSize, Vec2 artSize, float spacingX, float spacingY)
    : cell_(cellSize), art_(artSize), spacingX_(spacingX), spacingY_(spacingY) {}

void SkeletonGrid::update(float dt) {
    phase_ = std::fmod(phase_ + dt / kPeriod, 1.0f);
    View::update(dt);
}

void SkeletonGrid::draw(Renderer& r) {
    const float padX = theme::layout::marginX;
    const float padTop = theme::layout::focusOverscan;
    const float avail = width() - 2 * padX;
    const int columns = std::max(1, static_cast<int>(std::floor((avail + spacingX_) / (cell_.x + spacingX_))));
    const float used = static_cast<float>(columns) * cell_.x + static_cast<float>(columns - 1) * spacingX_;
    const float x0 = padX + std::max(0.0f, (avail - used) * 0.5f);
    for (int row = 0;; ++row) {
        const float y = padTop + static_cast<float>(row) * (cell_.y + spacingY_);
        if (y > height()) break;
        for (int col = 0; col < columns; ++col) {
            const float x = x0 + static_cast<float>(col) * (cell_.x + spacingX_);
            const float ph = std::fmod(phase_ + static_cast<float>(col + row) * 0.04f, 1.0f);
            r.fillShimmer({x, y, art_.x, art_.y}, theme::card::radius, theme::color::skeleton, ph);
            if (cell_.y > art_.y + 24)
                r.fillShimmer({x + 2, y + art_.y + 12, art_.x * 0.7f, 12}, 5, theme::color::skeleton, ph);
        }
    }
}

}  // namespace karafin::screens
