#include "ui/scroll.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ui/focus.hpp"
#include "ui/root.hpp"
#include "ui/theme.hpp"

namespace karafin::ui {
namespace {
constexpr float kFlingDecay = 2.6f;      // 1/s, exponential velocity decay
constexpr float kMinFlingVelocity = 18;  // px/s
}  // namespace

ScrollView::ScrollView(Axis axis) : axis_(axis) {
    auto box = std::make_unique<Box>(axis);
    box->setAlign(Align::Stretch);
    box_ = box.get();
    content_ = addChild(std::move(box));
    setCullMargin(0);
}

ScrollView::ScrollView(Axis axis, std::unique_ptr<View> customContent) : axis_(axis) {
    content_ = addChild(std::move(customContent));
    setCullMargin(0);
}

float ScrollView::maxScroll() const { return std::max(0.0f, contentSize_ - viewportSize()); }

float ScrollView::scrollTarget() const { return mode_ == Mode::Spring ? spring_.target() : position_; }

Vec2 ScrollView::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    Vec2 out{pref.x >= 0 ? pref.x : available.x, pref.y >= 0 ? pref.y : available.y};
    if (axis_ == Axis::Horizontal && pref.y < 0 && content_) {
        out.y = content_->measure({std::numeric_limits<float>::infinity(), available.y}).y;
    }
    return out;
}

void ScrollView::layoutContent() {
    if (!box_) return;
    if (axis_ == Axis::Vertical) {
        const float h = box_->measure({width(), std::numeric_limits<float>::infinity()}).y;
        box_->setFrame({0, 0, width(), std::max(h, height())});
        contentSize_ = h;
    } else {
        const float w = box_->measure({std::numeric_limits<float>::infinity(), height()}).x;
        box_->setFrame({0, 0, std::max(w, width()), height()});
        contentSize_ = w;
    }
}

void ScrollView::layout() {
    layoutContent();
    // Keep the position valid after content shrinks.
    if (mode_ == Mode::Idle && position_ > maxScroll()) {
        position_ = maxScroll();
        applyPosition();
    }
}

void ScrollView::applyPosition() {
    setContentOffset(axis_ == Axis::Horizontal ? Vec2{position_, 0} : Vec2{0, position_});
    if (position_ != lastApplied_) {
        lastApplied_ = position_;
        idleTime_ = 0;
        onScrollChanged();
        if (onScroll) onScroll(position_);
    }
}

void ScrollView::scrollTo(float offset, bool animated) {
    offset = std::clamp(offset, 0.0f, maxScroll());
    if (!animated) {
        mode_ = Mode::Idle;
        position_ = offset;
        spring_.set(offset);
        applyPosition();
        return;
    }
    if (mode_ != Mode::Spring) {
        spring_.set(position_);
        spring_.setVelocity(mode_ == Mode::Fling ? velocity_ : 0.0f);
    }
    spring_.springTo(offset, theme::motion::scroll);
    mode_ = Mode::Spring;
}

void ScrollView::scrollBy(float delta, bool animated) { scrollTo(scrollTarget() + delta, animated); }

void ScrollView::revealRect(const Rect& r, bool animated) {
    const float start = axis_ == Axis::Horizontal ? r.x : r.y;
    const float end = axis_ == Axis::Horizontal ? r.right() : r.bottom();
    const float vp = viewportSize();
    const float current = scrollTarget();
    float target = current;
    switch (anchor_) {
        case ScrollAnchor::Minimal:
            if (start - revealLeading_ < current) target = start - revealLeading_;
            else if (end + revealTrailing_ > current + vp) target = end + revealTrailing_ - vp;
            break;
        case ScrollAnchor::Leading: target = start - revealLeading_; break;
        case ScrollAnchor::Center: target = (start + end) * 0.5f - vp * 0.5f; break;
    }
    target = std::clamp(target, 0.0f, maxScroll());
    if (std::fabs(target - current) > 0.5f) scrollTo(target, animated);
}

void ScrollView::reveal(View* v, bool animated) {
    if (!v || !content_ || !v->isDescendantOf(content_)) return;
    if (revealWholeChild_) {
        while (v->parent() && v->parent() != content_) v = v->parent();
    }
    const Vec2 base = content_->rootPosition();
    const Rect rr = v->rootRect();
    revealRect(rr.translated(-base.x, -base.y), animated);
}

void ScrollView::onFocusWithin(View* descendant) {
    pendingReveal_ = descendant;
    pendingAnimated_ = true;
}

float ScrollView::rubberBand(float raw) const {
    const float maxS = maxScroll();
    const float vp = std::max(1.0f, viewportSize());
    auto band = [vp](float over) { return (1.0f - 1.0f / (over * 0.55f / vp + 1.0f)) * vp; };
    if (raw < 0) return -band(-raw);
    if (raw > maxS) return maxS + band(raw - maxS);
    return raw;
}

void ScrollView::dragBegin() {
    mode_ = Mode::Drag;
    // Invert the rubber band so grabbing an overscrolled list does not jump.
    const float maxS = maxScroll();
    const float vp = std::max(1.0f, viewportSize());
    auto unband = [vp](float shown) {
        const float f = std::min(0.95f, shown / vp);
        return (1.0f / (1.0f - f) - 1.0f) * vp / 0.55f;
    };
    if (position_ < 0) rawDrag_ = -unband(-position_);
    else if (position_ > maxS) rawDrag_ = maxS + unband(position_ - maxS);
    else rawDrag_ = position_;
    velocity_ = 0;
}

void ScrollView::dragMove(float fingerDelta) {
    if (mode_ != Mode::Drag) dragBegin();
    rawDrag_ -= fingerDelta;
    position_ = rubberBand(rawDrag_);
    applyPosition();
}

void ScrollView::dragEnd(float fingerVelocity) {
    velocity_ = -fingerVelocity;
    const float maxS = maxScroll();
    if (position_ < 0 || position_ > maxS) {
        spring_.set(position_);
        spring_.setVelocity(velocity_);
        spring_.springTo(std::clamp(position_, 0.0f, maxS), theme::motion::scroll);
        mode_ = Mode::Spring;
    } else if (std::fabs(velocity_) > kMinFlingVelocity) {
        mode_ = Mode::Fling;
    } else {
        mode_ = Mode::Idle;
    }
}

void ScrollView::update(float dt) {
    View::update(dt);

    if (View* v = pendingReveal_.get()) {
        pendingReveal_.reset();
        if (mode_ != Mode::Drag) reveal(v, pendingAnimated_);
    }

    switch (mode_) {
        case Mode::Idle:
        case Mode::Drag:
            break;
        case Mode::Spring:
            spring_.update(dt);
            position_ = spring_.value();
            if (!spring_.animating()) mode_ = Mode::Idle;
            applyPosition();
            break;
        case Mode::Fling: {
            position_ += velocity_ * dt;
            velocity_ *= std::exp(-kFlingDecay * dt);
            const float maxS = maxScroll();
            if (position_ < 0 || position_ > maxS) {
                // Hit an edge: hand the momentum to a spring that bounces back.
                spring_.set(position_);
                spring_.setVelocity(velocity_ * 0.6f);
                spring_.springTo(std::clamp(position_, 0.0f, maxS), theme::motion::scroll);
                mode_ = Mode::Spring;
            } else if (std::fabs(velocity_) < kMinFlingVelocity) {
                mode_ = Mode::Idle;
            }
            applyPosition();
            break;
        }
    }

    idleTime_ += dt;
    const float targetAlpha = (canScroll() && idleTime_ < 0.9f) ? 1.0f : 0.0f;
    indicatorAlpha_ += (targetAlpha - indicatorAlpha_) * std::min(1.0f, dt * (targetAlpha > indicatorAlpha_ ? 14.0f : 4.0f));
}

View* ScrollView::pageTarget(float delta) const {
    if (!Root::exists() || !content_) return nullptr;
    View* from = Root::instance().focus().focused();
    if (!from || !from->isDescendantOf(content_)) return nullptr;
    const Rect fr = from->rootRect();
    const Vec2 goal = axis_ == Axis::Vertical ? Vec2{fr.center().x, fr.center().y + delta}
                                              : Vec2{fr.center().x + delta, fr.center().y};
    View* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    std::vector<View*> stack{content_};
    while (!stack.empty()) {
        View* v = stack.back();
        stack.pop_back();
        if (!v->visible()) continue;
        if (v != from && v->canFocus()) {
            const Vec2 c = v->rootRect().center();
            const float along = axis_ == Axis::Vertical ? (c.y - fr.center().y) : (c.x - fr.center().x);
            if ((delta > 0 && along > 1) || (delta < 0 && along < -1)) {
                const float dx = c.x - goal.x, dy = c.y - goal.y;
                const float score = axis_ == Axis::Vertical ? dy * dy + dx * dx * 0.25f : dx * dx + dy * dy * 0.25f;
                if (score < bestScore) {
                    bestScore = score;
                    best = v;
                }
            }
        }
        for (auto& c : v->children()) stack.push_back(c.get());
    }
    return best;
}

bool ScrollView::onInput(const InputEvent& e) {
    if (paging_ && (e.button == Key::ZL || e.button == Key::ZR) && e.pressedOrRepeat() && canScroll()) {
        const float delta = (e.button == Key::ZR ? 1.0f : -1.0f) * viewportSize() * 0.85f;
        if (View* t = pageTarget(delta)) {
            Root::instance().focus().setFocus(t);
            return true;
        }
        scrollBy(delta);
        return true;
    }
    return View::onInput(e);
}

void ScrollView::drawOverlay(Renderer& r) {
    if (!showsIndicator_ || indicatorAlpha_ < 0.01f || contentSize_ <= 0) return;
    const float vp = viewportSize();
    const float len = std::max(28.0f, vp * vp / std::max(vp, contentSize_));
    const float maxS = std::max(1.0f, maxScroll());
    const float t = std::clamp(position_ / maxS, 0.0f, 1.0f);
    const Color c = Color::white(0.35f * indicatorAlpha_);
    if (axis_ == Axis::Vertical) {
        const float y = 6 + (vp - 12 - len) * t;
        r.fillRoundRect({width() - 7, y, 3.5f, len}, 2, c);
    } else {
        const float x = 6 + (vp - 12 - len) * t;
        r.fillRoundRect({x, height() - 7, len, 3.5f}, 2, c);
    }
}

}  // namespace karafin::ui
