#pragma once

#include <vector>

#include "ui/theme.hpp"
#include "ui/view.hpp"
#include "ui/widgets/poster_card.hpp"

namespace karafin::screens {

// Shimmering placeholder shapes (loading skeletons). Rects are in local coordinates; the view
// measures to their bounding box.
class Skeleton : public ui::View {
public:
    Skeleton() = default;
    Skeleton& addRect(const ui::Rect& r, float radius = 6);
    void clearRects() { rects_.clear(); }

    ui::Vec2 measure(ui::Vec2 available) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    struct Shape {
        ui::Rect rect;
        float radius;
    };
    std::vector<Shape> rects_;
    float phase_ = 0;
};

// Placeholder for a RowCarousel of `count` cards while the row loads.
class SkeletonRow : public ui::View {
public:
    explicit SkeletonRow(ui::CardShape shape = ui::CardShape::Poster, int count = 8, bool title = true);

    ui::Vec2 measure(ui::Vec2 available) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    ui::CardShape shape_;
    int count_;
    bool title_;
    float phase_ = 0;
};

// Placeholder for a ui::Grid while its first page loads: fills the frame with card shapes laid
// out like the grid (same cell size, spacing and default padding).
class SkeletonGrid : public ui::View {
public:
    SkeletonGrid(ui::Vec2 cellSize, ui::Vec2 artSize, float spacingX = ui::theme::layout::cardSpacing,
                 float spacingY = 30);

    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    ui::Vec2 cell_;
    ui::Vec2 art_;
    float spacingX_;
    float spacingY_;
    float phase_ = 0;
};

}  // namespace karafin::screens
