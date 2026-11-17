#pragma once

// Screens and the navigation stack.
//
//   class HomeScreen : public ui::Screen {
//   public:
//       HomeScreen() { /* build children */ }
//       void onShow() override { /* became top: refresh data */ }
//   };
//   ui::nav().push(std::make_unique<DetailScreen>(id));        // slide in
//   ui::nav().pop();                                            // B does this by default
//   ui::nav().resetTo(std::make_unique<HomeScreen>(), ui::Transition::Fade);  // after login
//
// Screens fill the 1280x720 logical viewport. During transitions both screens render into
// offscreen targets that are composited with slide/fade/zoom, so group opacity is correct.

#include <functional>
#include <memory>
#include <vector>

#include <SDL_events.h>

#include "gfx/texture.hpp"
#include "ui/view.hpp"

namespace karafin::ui {

class Screen : public View {
public:
    Screen();

    // Lifecycle (main thread):
    virtual void onPushed() {}  // added to the stack, before its transition starts
    virtual void onShow() {}    // became the top screen (after the transition)
    virtual void onHide() {}    // covered by another screen, or about to be popped
    virtual void onPopped() {}  // removed; destroyed once the transition ends
    // B pressed and not handled by the focused chain. Return true to consume; otherwise the
    // navigator pops (when not the root screen).
    virtual bool onBack() { return false; }

    // Opaque screens paint the theme background and hide screens below them.
    void setOpaque(bool o) { opaque_ = o; }
    bool opaque() const { return opaque_; }
    void setBackgroundColor(Color c) { bgColor_ = c; }
    void setShowsHintBar(bool v) { showsHintBar_ = v; }
    bool showsHintBar() const { return showsHintBar_; }
    void setNavigationSounds(bool v) { navigationSounds_ = v; }
    bool navigationSounds() const { return navigationSounds_; }

    // Screen-wide button handlers, run when the focused chain did not consume the event.
    void addShortcut(Key b, std::function<void()> fn);
    bool isTop() const;
    // + with no modal open (browsing screens show the app menu). Unset: + asks to quit.
    std::function<void()> onAppMenu;

    bool onInput(const InputEvent& e) override;
    // A screen with custom gestures may consume the entire touch stream instead of widget taps.
    virtual bool onTouchEvent(const SDL_Event&) { return false; }
    void draw(Renderer& r) override;

private:
    struct Shortcut {
        Key button;
        std::function<void()> fn;
    };
    std::vector<Shortcut> shortcuts_;
    Color bgColor_;
    bool opaque_ = true;
    bool showsHintBar_ = true;
    bool navigationSounds_ = true;
};

enum class Transition : uint8_t {
    None,
    Push,  // slide + fade (pop plays it in reverse)
    Fade,  // crossfade
    Zoom,  // scale + fade, for "entering" content (detail pages, player)
    Up,    // slide up + fade, for sheet-like pages
};

class Navigator {
public:
    void push(std::unique_ptr<Screen> screen, Transition t = Transition::Push);
    void pop(Transition t = Transition::Push);
    void replace(std::unique_ptr<Screen> screen, Transition t = Transition::Fade);
    void resetTo(std::unique_ptr<Screen> screen, Transition t = Transition::Fade);
    // Pops everything above `screen` (no-op if it is not in the stack).
    void popTo(Screen* screen, Transition t = Transition::Push);
    void popToRoot(Transition t = Transition::Push);

    Screen* top() const { return stack_.empty() ? nullptr : stack_.back().get(); }
    Screen* at(size_t i) const { return i < stack_.size() ? stack_[i].get() : nullptr; }
    size_t depth() const { return stack_.size(); }
    bool transitioning() const { return animating_; }

    // Driven by ui::Root.
    void layout(Vec2 size);
    void update(float dt);
    void render(Renderer& r);
    void releaseTargets();
    void clear();

private:
    void begin(Screen* from, Screen* to, Transition t, bool reverse);
    void finish();
    void renderComposite(Renderer& r);
    void renderBelow(Renderer& r, Screen* s);

    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> dying_;
    std::vector<std::unique_ptr<Screen>> graveyard_;
    Screen* from_ = nullptr;
    Screen* to_ = nullptr;
    Transition type_ = Transition::None;
    bool reverse_ = false;
    bool animating_ = false;
    float t_ = 0;
    float duration_ = 0.42f;
    float targetIdle_ = 0;  // seconds since the transition targets were last used
    bool fromCaptured_ = false;  // rtFrom_ holds the outgoing screen for this transition
    int capturedW_ = 0, capturedH_ = 0;
    Vec2 size_{1280, 720};
    gfx::RenderTarget rtFrom_;
    gfx::RenderTarget rtTo_;
};

}  // namespace karafin::ui
