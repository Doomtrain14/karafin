#pragma once

// Retained widget tree.
//
// A View has a frame (position + size in its parent's content space, logical px), owns its
// children (unique_ptr), and is drawn every frame by render(): transform (frame origin + animated
// offset/scale) and alpha are pushed, then draw(), children (z-sorted, culled), drawOverlay().
//
// Lifetime safety: async callbacks must not capture raw `this` blindly. Use
//   http.send(req, guard([this](auto&& resp) { ... }));   // runs only while the view is alive
// or keep a ui::Ref<T> (weak handle) to another view.
//
// Main thread only.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/renderer.hpp"
#include "gfx/types.hpp"
#include "ui/anim.hpp"
#include "ui/input.hpp"

namespace karafin::ui {

using gfx::Color;
using gfx::Insets;
using gfx::Rect;
using gfx::Renderer;
using gfx::Vec2;

class Screen;
class View;

// Weak handle to a view: get() returns null once the view is destroyed.
template <class T>
class Ref {
public:
    Ref() = default;
    Ref(T* v);  // NOLINT(google-explicit-constructor)
    T* get() const { return alive_.expired() ? nullptr : ptr_; }
    T* operator->() const { return get(); }
    explicit operator bool() const { return get() != nullptr; }
    void reset() {
        ptr_ = nullptr;
        alive_.reset();
    }

private:
    T* ptr_ = nullptr;
    std::weak_ptr<void> alive_;
};

class View {
public:
    View();
    virtual ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // ---- Tree ---------------------------------------------------------------------------
    template <class T, class... Args>
    T* add(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = child.get();
        addChild(std::move(child));
        return raw;
    }
    View* addChild(std::unique_ptr<View> child);
    View* insertChild(size_t index, std::unique_ptr<View> child);
    std::unique_ptr<View> removeChild(View* child);
    void clearChildren();
    View* parent() const { return parent_; }
    const std::vector<std::unique_ptr<View>>& children() const { return children_; }
    size_t childCount() const { return children_.size(); }
    View* childAt(size_t i) const { return i < children_.size() ? children_[i].get() : nullptr; }
    size_t indexOf(const View* child) const;
    // True if this == ancestor or ancestor contains this.
    bool isDescendantOf(const View* ancestor) const;
    Screen* screen() const;

    void setName(std::string name) { name_ = std::move(name); }
    const std::string& name() const { return name_; }

    // Alive token for weak captures (Timeline owners, Ref<T>).
    std::weak_ptr<void> lifetime() const { return alive_; }
    template <class F>
    auto guard(F f) const {
        return [w = lifetime(), f = std::move(f)](auto&&... args) mutable {
            if (!w.expired()) f(std::forward<decltype(args)>(args)...);
        };
    }

    // ---- Geometry -----------------------------------------------------------------------
    const Rect& frame() const { return frame_; }
    void setFrame(const Rect& r);
    void setPosition(float x, float y) { setFrame({x, y, frame_.w, frame_.h}); }
    void setSize(float w, float h) { setFrame({frame_.x, frame_.y, w, h}); }
    float width() const { return frame_.w; }
    float height() const { return frame_.h; }
    Rect bounds() const { return {0, 0, frame_.w, frame_.h}; }

    // Scroll offset applied to children (content space -> local space subtracts it).
    Vec2 contentOffset() const { return contentOffset_; }
    void setContentOffset(Vec2 o) { contentOffset_ = o; }

    // Layout position in root (screen) coordinates. Includes ancestors' scroll offsets, excludes
    // transient animation offset/scale. Used for focus navigation and hit testing.
    Vec2 rootPosition() const;
    Rect rootRect() const { return {rootPosition().x, rootPosition().y, frame_.w, frame_.h}; }
    Vec2 toLocal(Vec2 rootPoint) const { return rootPoint - rootPosition(); }

    // ---- Layout -------------------------------------------------------------------------
    // Preferred size for parent layouts; negative = automatic (measure()).
    void setPreferredSize(float w, float h);
    Vec2 preferredSize() const { return preferred_; }
    void setFlex(float f);
    float flex() const { return flex_; }
    void setMargin(const Insets& m);
    const Insets& margin() const { return margin_; }

    // Natural size given the available space. Default: preferred size, auto dimensions fall back
    // to the current frame size.
    virtual Vec2 measure(Vec2 available);
    // Positions children inside the current frame. Default: nothing (manual frames).
    virtual void layout() {}
    void setNeedsLayout();
    bool needsLayout() const { return needsLayout_; }
    // Runs layout() where flagged, recursively. Clean subtrees are skipped (frame size changes and
    // setNeedsLayout() flag the path to the root), so calling it every frame is cheap.
    void layoutIfNeeded();

    // ---- Appearance ---------------------------------------------------------------------
    // Transient, animatable presentation (does not affect layout or focus geometry).
    Animated<float> alpha{1.0f};
    Animated<float> scale{1.0f};
    Animated<Vec2> offset{Vec2{0, 0}};
    void setPivot(Vec2 normalized) { pivot_ = normalized; }  // scale origin, default center

    void setVisible(bool v);
    bool visible() const { return visible_; }
    bool visibleInTree() const;
    void setClipsChildren(bool c) { clips_ = c; }
    bool clipsChildren() const { return clips_; }
    // Sibling draw order (higher draws later / on top). Focused cards raise themselves.
    void setZ(float z);
    float z() const { return z_; }
    void setBackground(Color c, float radius = 0) {
        background_ = c;
        backgroundRadius_ = radius;
    }
    // Extra margin used when culling children against the clip (shadows, focus growth).
    void setCullMargin(float m) { cullMargin_ = m; }

    // ---- Per frame ----------------------------------------------------------------------
    // Ticks animated properties and children. Overrides must call View::update(dt).
    virtual void update(float dt);
    void render(Renderer& r);
    virtual void draw(Renderer& r) { (void)r; }
    virtual void drawOverlay(Renderer& r) { (void)r; }

    // ---- Focus --------------------------------------------------------------------------
    void setFocusable(bool f) { focusable_ = f; }
    bool focusable() const { return focusable_; }
    void setEnabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }
    virtual bool canFocus() const { return focusable_ && enabled_ && visibleInTree(); }
    bool focused() const { return focused_; }
    bool hasFocusWithin() const;
    void requestFocus();
    // Identity that survives a rebuild. Focus landing on a new view with the same id does not
    // replay the focus-gain animation (the previous view may already have been destroyed).
    void setFocusId(std::string id) { focusId_ = std::move(id); }
    const std::string& focusId() const { return focusId_; }

    virtual void onFocus() {}
    virtual void onBlur() {}
    // Called on every ancestor of the newly focused view.
    virtual void onFocusWithin(View* descendant) { (void)descendant; }
    // Called on ancestors of the previous focus that no longer contain focus.
    virtual void onFocusLeft() {}

    // Container hook for directional moves that start inside this subtree. Return a view to
    // focus (it or its defaultFocus() is used), focusBlocked() to swallow the move, or null to let
    // the parent / spatial search decide.
    virtual View* navigateFocus(View* from, NavDir dir) {
        (void)from;
        (void)dir;
        return nullptr;
    }
    static View* focusBlocked();

    // Where focus goes when entering this subtree. Default: itself if focusable, else the
    // remembered descendant (see setRememberFocus), else the first focusable descendant.
    virtual View* defaultFocus();
    void setRememberFocus(bool r) { rememberFocus_ = r; }
    bool remembersFocus() const { return rememberFocus_; }
    View* rememberedFocus() const { return remembered_.get(); }
    void setRememberedFocus(View* v) { remembered_ = v; }

    // ---- Input --------------------------------------------------------------------------
    // Bubbles from the focused view up through its ancestors; return true to consume.
    virtual bool onInput(const InputEvent& e);
    // Activation (A on the focused view, or tap). Default calls onActivate.
    virtual bool activate();
    std::function<void()> onActivate;

    // Hints shown in the hint bar while this view (or a descendant) has focus.
    void setHints(std::vector<Hint> hints) { hints_ = std::move(hints); }
    const std::vector<Hint>& hints() const { return hints_; }

    // Touch. Points are in this view's local coordinates.
    virtual bool hitTest(Vec2 local) const { return bounds().contains(local); }
    // Tap on this view (deepest hit first, bubbling to ancestors until one returns true).
    // Default: focusable views take focus and activate.
    virtual bool onTap(Vec2 local);
    // Touch-down highlight feedback (optional).
    virtual void onTouchDown() {}
    virtual void onTouchCancel() {}
    // Deepest visible view containing `rootPoint` (children before parents, top z first).
    View* hitTestDeep(Vec2 rootPoint);

protected:
    // Pushes this view's local transform (frame origin + offset + scale) to the renderer.
    gfx::Affine localTransform() const;
    // True only inside onFocus when this view is taking over for one with the same focusId.
    bool focusGainSilent() const { return focusGainSilent_; }

private:
    friend class FocusManager;
    void renderChildren(Renderer& r);
    void setParent(View* p) { parent_ = p; }
    void markSubtreeDirty();
    void sortedByZ(std::vector<View*>& out) const;

    std::shared_ptr<char> alive_;
    View* parent_ = nullptr;
    std::vector<std::unique_ptr<View>> children_;
    std::vector<View*> zOrder_;  // scratch for z-sorted rendering (reused, no per-frame allocation)
    std::string name_;
    Rect frame_;
    Vec2 contentOffset_;
    Vec2 preferred_{-1, -1};
    Vec2 pivot_{0.5f, 0.5f};
    Insets margin_;
    float flex_ = 0;
    float z_ = 0;
    float cullMargin_ = 48;
    Color background_ = Color::transparent();
    float backgroundRadius_ = 0;
    std::vector<Hint> hints_;
    Ref<View> remembered_;
    bool visible_ = true;
    bool clips_ = false;
    bool focusable_ = false;
    bool enabled_ = true;
    bool focused_ = false;
    bool focusGainSilent_ = false;
    bool rememberFocus_ = false;
    std::string focusId_;
    bool needsLayout_ = true;
    bool subtreeDirty_ = false;  // a descendant needs layout
    bool hasZ_ = false;
};

template <class T>
Ref<T>::Ref(T* v) : ptr_(v) {
    if (v) alive_ = v->lifetime();
}

}  // namespace karafin::ui
