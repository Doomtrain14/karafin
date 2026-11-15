#include "ui/view.hpp"

#include <algorithm>

#include "ui/focus.hpp"
#include "ui/root.hpp"
#include "ui/screen.hpp"

namespace karafin::ui {

View::View() : alive_(std::make_shared<char>(0)) {}

View::~View() {
    alive_.reset();
    children_.clear();
}

View* View::addChild(std::unique_ptr<View> child) { return insertChild(children_.size(), std::move(child)); }

View* View::insertChild(size_t index, std::unique_ptr<View> child) {
    if (!child) return nullptr;
    View* raw = child.get();
    raw->setParent(this);
    if (raw->z_ != 0) hasZ_ = true;
    index = std::min(index, children_.size());
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
    setNeedsLayout();
    return raw;
}

std::unique_ptr<View> View::removeChild(View* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<View> out = std::move(*it);
            children_.erase(it);
            out->setParent(nullptr);
            setNeedsLayout();
            return out;
        }
    }
    return nullptr;
}

void View::clearChildren() {
    children_.clear();
    setNeedsLayout();
}

size_t View::indexOf(const View* child) const {
    for (size_t i = 0; i < children_.size(); ++i)
        if (children_[i].get() == child) return i;
    return SIZE_MAX;
}

bool View::isDescendantOf(const View* ancestor) const {
    for (const View* v = this; v; v = v->parent_)
        if (v == ancestor) return true;
    return false;
}

Screen* View::screen() const {
    for (const View* v = this; v; v = v->parent_)
        if (auto* s = dynamic_cast<const Screen*>(v)) return const_cast<Screen*>(s);
    return nullptr;
}

void View::setFrame(const Rect& r) {
    if (r.w != frame_.w || r.h != frame_.h) {
        needsLayout_ = true;
        markSubtreeDirty();
    }
    frame_ = r;
}

void View::markSubtreeDirty() {
    for (View* v = parent_; v; v = v->parent_) v->subtreeDirty_ = true;
}

Vec2 View::rootPosition() const {
    Vec2 p{frame_.x, frame_.y};
    for (const View* v = parent_; v; v = v->parent_) {
        p.x += v->frame_.x - v->contentOffset_.x;
        p.y += v->frame_.y - v->contentOffset_.y;
    }
    return p;
}

void View::setPreferredSize(float w, float h) {
    if (preferred_.x == w && preferred_.y == h) return;
    preferred_ = {w, h};
    if (parent_) parent_->setNeedsLayout();
}

void View::setFlex(float f) {
    if (flex_ == f) return;
    flex_ = f;
    if (parent_) parent_->setNeedsLayout();
}

void View::setMargin(const Insets& m) {
    margin_ = m;
    if (parent_) parent_->setNeedsLayout();
}

Vec2 View::measure(Vec2 available) {
    (void)available;
    return {preferred_.x >= 0 ? preferred_.x : frame_.w, preferred_.y >= 0 ? preferred_.y : frame_.h};
}

void View::setNeedsLayout() {
    needsLayout_ = true;
    for (View* v = parent_; v && !v->needsLayout_; v = v->parent_) v->needsLayout_ = true;
    markSubtreeDirty();
}

void View::layoutIfNeeded() {
    if (!needsLayout_ && !subtreeDirty_) return;
    if (needsLayout_) {
        needsLayout_ = false;
        layout();
    }
    subtreeDirty_ = false;
    // Index loop: a child's layout() may add children to this view (e.g. grid cells).
    for (size_t i = 0; i < children_.size(); ++i) {
        View* c = children_[i].get();
        if (c->visible_) c->layoutIfNeeded();
    }
}

void View::setVisible(bool v) {
    if (visible_ == v) return;
    visible_ = v;
    if (parent_) parent_->setNeedsLayout();
}

bool View::visibleInTree() const {
    for (const View* v = this; v; v = v->parent_)
        if (!v->visible_) return false;
    return true;
}

void View::setZ(float z) {
    z_ = z;
    if (parent_ && z != 0) parent_->hasZ_ = true;
}

void View::update(float dt) {
    alpha.update(dt);
    scale.update(dt);
    offset.update(dt);
    // Index loop: children may be added/removed by callbacks during update.
    for (size_t i = 0; i < children_.size(); ++i) {
        View* c = children_[i].get();
        if (c->visible_) c->update(dt);
    }
}

gfx::Affine View::localTransform() const {
    const Vec2 off = offset.value();
    gfx::Affine t = gfx::Affine::translate(frame_.x + off.x, frame_.y + off.y);
    const float s = scale.value();
    if (s != 1.0f) t = t * gfx::Affine::scaleAround(s, {frame_.w * pivot_.x, frame_.h * pivot_.y});
    return t;
}

void View::render(Renderer& r) {
    if (!visible_) return;
    const float a = alpha.value();
    if (a <= 0.003f) return;
    r.pushTransform(localTransform());
    r.pushAlpha(a);
    if (background_.a > 0) r.fillRoundRect(bounds(), backgroundRadius_, background_);
    draw(r);
    if (!children_.empty()) {
        if (clips_) r.pushClip(bounds());
        const bool scrolled = contentOffset_.x != 0 || contentOffset_.y != 0;
        if (scrolled) r.pushTranslate(-contentOffset_.x, -contentOffset_.y);
        renderChildren(r);
        if (scrolled) r.popTransform();
        if (clips_) r.popClip();
    }
    drawOverlay(r);
    r.popAlpha();
    r.popTransform();
}

void View::renderChildren(Renderer& r) {
    auto drawChild = [&](View* c) {
        if (!c->visible_) return;
        const Vec2 off = c->offset.value();
        const Rect f = c->frame_.translated(off).expanded(c->cullMargin_ * std::max(1.0f, c->scale.value()));
        if (!r.isVisible(f)) return;
        c->render(r);
    };
    if (!hasZ_) {
        for (auto& c : children_) drawChild(c.get());
        return;
    }
    // Member scratch: render() never re-enters the same view, so reuse is safe.
    sortedByZ(zOrder_);
    for (View* c : zOrder_) drawChild(c);
    zOrder_.clear();
}

void View::sortedByZ(std::vector<View*>& out) const {
    out.clear();
    for (auto& c : children_) out.push_back(c.get());
    // Insertion sort: stable, allocation-free (std::stable_sort allocates), nearly sorted input.
    for (size_t i = 1; i < out.size(); ++i) {
        View* v = out[i];
        size_t j = i;
        for (; j > 0 && out[j - 1]->z_ > v->z_; --j) out[j] = out[j - 1];
        out[j] = v;
    }
}

bool View::hasFocusWithin() const {
    if (!Root::exists()) return false;
    View* f = Root::instance().focus().focused();
    return f && f->isDescendantOf(this);
}

void View::requestFocus() {
    if (Root::exists()) Root::instance().focus().setFocus(this);
}

View* View::focusBlocked() {
    static View sentinel;
    return &sentinel;
}

View* View::defaultFocus() {
    if (!visible_) return nullptr;
    if (canFocus()) return this;
    if (rememberFocus_) {
        View* m = remembered_.get();
        if (m && m->isDescendantOf(this) && m->canFocus()) return m;
    }
    for (auto& c : children_)
        if (View* f = c->defaultFocus()) return f;
    return nullptr;
}

bool View::onInput(const InputEvent& e) {
    if (e.button == Key::A && e.pressed() && focused_) return activate();
    return false;
}

bool View::activate() {
    if (!enabled_) return false;
    if (onActivate) {
        onActivate();
        return true;
    }
    return false;
}

bool View::onTap(Vec2 local) {
    (void)local;
    if (!canFocus()) return false;
    requestFocus();
    activate();
    return true;
}

View* View::hitTestDeep(Vec2 rootPoint) {
    if (!visible_ || alpha.value() <= 0.01f) return nullptr;
    const Vec2 local = toLocal(rootPoint);
    if (clips_ && !bounds().contains(local)) return nullptr;
    // Children in reverse draw order.
    std::vector<View*> order;
    sortedByZ(order);
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (View* hit = (*it)->hitTestDeep(rootPoint)) return hit;
    return hitTest(local) ? this : nullptr;
}

}  // namespace karafin::ui
