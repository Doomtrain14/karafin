#pragma once

// Scrollable container along one axis.
//
//   auto* page = add<ScrollView>(Axis::Vertical);
//   page->content()->setSpacing(theme::layout::rowSpacing);
//   page->content()->add<RowCarousel>(...);
//
// - Focus inside scrolls it into view with a spring (anchor: Minimal / Leading / Center).
// - Touch: drag with rubber-band edges, fling with exponential deceleration.
// - ZL / ZR page through the content, moving focus along.
// - Subclasses can provide their own content view (virtualized grids) and call setContentSize().

#include "ui/layout.hpp"

namespace karafin::ui {

enum class ScrollAnchor : uint8_t {
    Minimal,  // move as little as possible to keep the item (plus reveal insets) visible
    Leading,  // align the item to the leading edge + leading inset (Apple TV rows)
    Center,   // center the item
};

class ScrollView : public View {
public:
    explicit ScrollView(Axis axis = Axis::Vertical);

    Axis axis() const { return axis_; }
    // The content stack (laid out along the scroll axis). Null for custom-content subclasses.
    Box* content() const { return box_; }
    View* contentView() const { return content_; }

    float scrollOffset() const { return position_; }
    float scrollTarget() const;
    float maxScroll() const;
    float viewportSize() const { return axis_ == Axis::Horizontal ? width() : height(); }
    float contentSize() const { return contentSize_; }
    bool canScroll() const { return maxScroll() > 0.5f; }

    void scrollTo(float offset, bool animated = true);
    void scrollBy(float delta, bool animated = true);
    // Rect in content coordinates.
    void revealRect(const Rect& contentRect, bool animated = true);
    void reveal(View* descendant, bool animated = true);

    void setAnchor(ScrollAnchor a) { anchor_ = a; }
    // Space kept before/after a revealed item (also room for focus scale-up).
    void setRevealInsets(float leading, float trailing) {
        revealLeading_ = leading;
        revealTrailing_ = trailing;
    }
    // Reveal the content's direct child containing the focused view (whole rows on a page).
    void setRevealWholeChild(bool v) { revealWholeChild_ = v; }
    void setShowsIndicator(bool v) { showsIndicator_ = v; }
    void setPagingEnabled(bool v) { paging_ = v; }

    // Touch driving (called by the root's touch router). Deltas/velocities in logical px along
    // the axis, in finger direction.
    void dragBegin();
    void dragMove(float fingerDelta);
    void dragEnd(float fingerVelocity);
    bool dragging() const { return mode_ == Mode::Drag; }
    bool moving() const { return mode_ != Mode::Idle; }

    std::function<void(float)> onScroll;

    Vec2 measure(Vec2 available) override;
    void layout() override;
    void update(float dt) override;
    void onFocusWithin(View* descendant) override;
    bool onInput(const InputEvent& e) override;
    void drawOverlay(Renderer& r) override;

protected:
    ScrollView(Axis axis, std::unique_ptr<View> customContent);
    void setContentSize(float size) { contentSize_ = size; }
    // Called whenever the scroll position changed (virtualization hook).
    virtual void onScrollChanged() {}
    // Lays out the content view; default sizes the Box to its measured length.
    virtual void layoutContent();

private:
    enum class Mode : uint8_t { Idle, Spring, Drag, Fling };
    void applyPosition();
    float rubberBand(float raw) const;
    View* pageTarget(float delta) const;

    Axis axis_;
    View* content_ = nullptr;
    Box* box_ = nullptr;
    float contentSize_ = 0;
    float position_ = 0;
    float rawDrag_ = 0;
    float velocity_ = 0;
    Animated<float> spring_{0.0f};
    Mode mode_ = Mode::Idle;
    ScrollAnchor anchor_ = ScrollAnchor::Minimal;
    float revealLeading_ = 40;
    float revealTrailing_ = 40;
    bool revealWholeChild_ = false;
    bool showsIndicator_ = false;
    bool paging_ = true;
    Ref<View> pendingReveal_;
    bool pendingAnimated_ = true;
    float lastApplied_ = -1e9f;
    float indicatorAlpha_ = 0;
    float idleTime_ = 10;
};

}  // namespace karafin::ui
