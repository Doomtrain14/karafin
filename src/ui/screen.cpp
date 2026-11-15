#include "ui/screen.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "core/platform.hpp"
#include "ui/root.hpp"
#include "ui/theme.hpp"

namespace karafin::ui {

Screen::Screen() : bgColor_(theme::color::background) {
    setRememberFocus(true);
    setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
}

void Screen::addShortcut(Key b, std::function<void()> fn) { shortcuts_.push_back({b, std::move(fn)}); }

bool Screen::isTop() const { return Root::exists() && Root::instance().nav().top() == this; }

bool Screen::onInput(const InputEvent& e) {
    if (e.pressed()) {
        for (auto& s : shortcuts_) {
            if (s.button == e.button && s.fn) {
                auto fn = s.fn;  // may modify shortcuts_ or pop this screen
                fn();
                return true;
            }
        }
    }
    return View::onInput(e);
}

void Screen::draw(Renderer& r) {
    if (opaque_) r.fillRect(bounds(), bgColor_);
}

// ---------------------------------------------------------------------------------------------

void Navigator::push(std::unique_ptr<Screen> screen, Transition t) {
    if (!screen) return;
    if (animating_) finish();
    Screen* from = top();
    Screen* to = screen.get();
    to->setFrame({0, 0, size_.x, size_.y});
    stack_.push_back(std::move(screen));
    to->onPushed();
    to->layoutIfNeeded();
    if (from) from->onHide();
    begin(from, to, from ? t : Transition::None, false);
}

void Navigator::pop(Transition t) {
    if (stack_.size() <= 1) return;
    if (animating_) finish();
    std::unique_ptr<Screen> leaving = std::move(stack_.back());
    stack_.pop_back();
    Screen* from = leaving.get();
    from->onHide();
    from->onPopped();
    dying_.push_back(std::move(leaving));
    Screen* to = top();
    to->layoutIfNeeded();
    begin(from, to, t, true);
}

void Navigator::replace(std::unique_ptr<Screen> screen, Transition t) {
    if (!screen) return;
    if (animating_) finish();
    Screen* from = nullptr;
    if (!stack_.empty()) {
        std::unique_ptr<Screen> leaving = std::move(stack_.back());
        stack_.pop_back();
        from = leaving.get();
        from->onHide();
        from->onPopped();
        dying_.push_back(std::move(leaving));
    }
    Screen* to = screen.get();
    to->setFrame({0, 0, size_.x, size_.y});
    stack_.push_back(std::move(screen));
    to->onPushed();
    to->layoutIfNeeded();
    begin(from, to, from ? t : Transition::None, false);
}

void Navigator::resetTo(std::unique_ptr<Screen> screen, Transition t) {
    if (!screen) return;
    if (animating_) finish();
    Screen* from = top();
    if (from) from->onHide();
    while (!stack_.empty()) {
        std::unique_ptr<Screen> s = std::move(stack_.back());
        stack_.pop_back();
        s->onPopped();
        dying_.push_back(std::move(s));
    }
    Screen* to = screen.get();
    to->setFrame({0, 0, size_.x, size_.y});
    stack_.push_back(std::move(screen));
    to->onPushed();
    to->layoutIfNeeded();
    begin(from, to, from ? t : Transition::None, false);
}

void Navigator::popTo(Screen* screen, Transition t) {
    auto it = std::find_if(stack_.begin(), stack_.end(), [&](const auto& s) { return s.get() == screen; });
    if (it == stack_.end() || it + 1 == stack_.end()) return;
    if (animating_) finish();
    std::unique_ptr<Screen> leaving = std::move(stack_.back());
    stack_.pop_back();
    leaving->onHide();
    leaving->onPopped();
    Screen* from = leaving.get();
    dying_.push_back(std::move(leaving));
    while (stack_.back().get() != screen) {
        std::unique_ptr<Screen> s = std::move(stack_.back());
        stack_.pop_back();
        s->onPopped();
        dying_.push_back(std::move(s));
    }
    screen->layoutIfNeeded();
    begin(from, screen, t, true);
}

void Navigator::popToRoot(Transition t) {
    if (stack_.size() > 1) popTo(stack_.front().get(), t);
}

void Navigator::begin(Screen* from, Screen* to, Transition t, bool reverse) {
    from_ = from;
    to_ = to;
    type_ = t;
    reverse_ = reverse;
    t_ = 0;
    duration_ = theme::motion::screenTransition;
    fromCaptured_ = false;
    animating_ = from && to && t != Transition::None;
    if (!animating_) finish();
}

void Navigator::finish() {
    animating_ = false;
    Screen* shown = to_;
    from_ = to_ = nullptr;
    // Destroyed at the start of the next update: finish() can run from inside a dying screen's
    // own callbacks (e.g. it pushes during its update).
    for (auto& s : dying_) graveyard_.push_back(std::move(s));
    dying_.clear();
    if (shown && shown == top()) shown->onShow();
}

void Navigator::layout(Vec2 size) {
    size_ = size;
    for (auto& s : stack_) {
        s->setFrame({0, 0, size.x, size.y});
        s->layoutIfNeeded();
    }
    for (auto& s : dying_) s->layoutIfNeeded();
}

void Navigator::update(float dt) {
    graveyard_.clear();
    if (animating_) {
        targetIdle_ = 0;
        t_ += dt;
        if (from_) from_->update(dt);
        if (to_) to_->update(dt);
        if (t_ >= duration_) finish();
        return;
    }
    // Two full-screen RGBA targets are ~16 MiB at 1080p. Reallocating them made the first frame
    // of every transition (after a pause) hitch, so they are kept, except in applet mode where
    // memory is tight.
    if ((rtFrom_.valid() || rtTo_.valid()) && platform::isAppletMode()) {
        targetIdle_ += dt;
        if (targetIdle_ > 3.0f) releaseTargets();
    }
    if (Screen* s = top()) s->update(dt);
}

void Navigator::releaseTargets() {
    rtFrom_.release();
    rtTo_.release();
}

void Navigator::clear() {
    animating_ = false;
    from_ = to_ = nullptr;
    while (!stack_.empty()) {
        stack_.back()->onPopped();
        stack_.pop_back();
    }
    dying_.clear();
    graveyard_.clear();
    releaseTargets();
}

void Navigator::render(Renderer& r) {
    if (animating_) {
        renderComposite(r);
        return;
    }
    if (stack_.empty()) return;
    size_t first = stack_.size() - 1;
    while (first > 0 && !stack_[first]->opaque()) --first;
    for (size_t i = first; i < stack_.size(); ++i) stack_[i]->render(r);
}

void Navigator::renderBelow(Renderer& r, Screen* s) {
    // Screens under `s` (bottom-up) that show through it; `s` is non-opaque.
    size_t end = stack_.size();
    for (size_t i = 0; i < stack_.size(); ++i)
        if (stack_[i].get() == s || stack_[i].get() == to_) {
            end = i;
            break;
        }
    if (end == 0) return;
    size_t first = end - 1;
    while (first > 0 && !stack_[first]->opaque()) --first;
    for (size_t i = first; i < end; ++i)
        if (stack_[i].get() != from_ && stack_[i].get() != to_) stack_[i]->render(r);
}

void Navigator::renderComposite(Renderer& r) {
    const DisplayInfo& d = r.display();
    const float raw = std::clamp(t_ / duration_, 0.0f, 1.0f);
    // The lower screen (under the transition) and the upper one (entering on push, leaving on pop).
    Screen* lower = reverse_ ? to_ : from_;
    Screen* upper = reverse_ ? from_ : to_;
    if (!lower->opaque()) renderBelow(r, lower);
    // A non-opaque screen (overlay, OSD) slides over a lower screen that stays put.
    const bool overlay = !upper->opaque();
    gfx::RenderTarget& upperRt = reverse_ ? rtFrom_ : rtTo_;
    gfx::RenderTarget& lowerRt = reverse_ ? rtTo_ : rtFrom_;
    const bool ok = upperRt.ensure(d.pixelWidth, d.pixelHeight) &&
                    (overlay || lowerRt.ensure(d.pixelWidth, d.pixelHeight));
    if (!ok) {
        // No offscreen targets: hard cut.
        if (overlay || reverse_) lower->render(r);
        if (overlay || !reverse_) upper->render(r);
        return;
    }
    // The outgoing screen is captured once and then composited from its target: re-rendering a
    // screen nobody interacts with any more doubled the per-frame cost of every transition.
    auto renderInto = [&](gfx::RenderTarget& rt, Screen* s) {
        const bool outgoing = s == from_ && &rt == &rtFrom_;
        if (outgoing && fromCaptured_ && capturedW_ == rt.width() && capturedH_ == rt.height()) return;
        r.pushTarget(rt, true, Color::transparent());
        s->render(r);
        r.popTarget();
        if (outgoing) {
            fromCaptured_ = true;
            capturedW_ = rt.width();
            capturedH_ = rt.height();
        }
    };
    if (overlay) lower->render(r);
    else renderInto(lowerRt, lower);
    renderInto(upperRt, upper);

    const float p = ease::emphasized(raw);
    auto smooth = [](float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    const float inAlpha = smooth(0.0f, 0.7f, p);
    const float outAlpha = 1.0f - smooth(0.25f, 1.0f, p);
    const Rect full{0, 0, size_.x, size_.y};
    const Vec2 center = full.center();

    struct Layer {
        gfx::RenderTarget* rt;
        float alpha;
        Vec2 offset;
        float scale;
    };
    Layer outgoing{&rtFrom_, outAlpha, {}, 1.0f};
    Layer incoming{&rtTo_, inAlpha, {}, 1.0f};
    const float dir = reverse_ ? -1.0f : 1.0f;
    switch (type_) {
        case Transition::Push:
            incoming.offset = {dir * (1 - p) * 90.0f, 0};
            outgoing.offset = {-dir * p * 70.0f, 0};
            break;
        case Transition::Zoom:
            incoming.scale = reverse_ ? 1.04f - 0.04f * p : 0.94f + 0.06f * p;
            outgoing.scale = reverse_ ? 1.0f - 0.06f * p : 1.0f + 0.04f * p;
            break;
        case Transition::Up:
            if (reverse_) outgoing.offset = {0, p * 48.0f};
            else incoming.offset = {0, (1 - p) * 48.0f};
            break;
        case Transition::Fade:
        case Transition::None:
            break;
    }
    auto drawLayer = [&](const Layer& l) {
        if (l.alpha <= 0.003f) return;
        if (overlay && l.rt == &lowerRt) return;  // drawn directly above
        gfx::Affine t = gfx::Affine::translate(l.offset.x, l.offset.y);
        if (l.scale != 1.0f) t = t * gfx::Affine::scaleAround(l.scale, center);
        r.pushTransform(t);
        r.drawRenderTarget(*l.rt, full, l.alpha);
        r.popTransform();
    };
    // Popping: the revealed screen sits underneath the one leaving.
    if (reverse_) {
        drawLayer(incoming);
        drawLayer(outgoing);
    } else {
        drawLayer(outgoing);
        drawLayer(incoming);
    }
}

}  // namespace karafin::ui
