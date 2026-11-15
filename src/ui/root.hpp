#pragma once

// UI host: the karafin::AppDelegate that owns the renderer, screen stack, modal layer, toasts,
// focus engine, input mapping and the hint bar.
//
//   int main() { ... App app; app.run(std::make_unique<ui::Root>([] { return std::make_unique<MyScreen>(); })); }
//
// Frame order (main thread): SDL events -> input dispatch -> timeline -> image cache ->
// layout -> update(dt) -> focus validation -> render (screens, modals, toasts, hint bar).
//
// Input routing for a button event: focused view -> its ancestors -> scope (modal or screen).
// Unconsumed directions move focus; unconsumed B pops the screen (Screen::onBack first);
// unconsumed + asks to quit.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/app.hpp"
#include "gfx/renderer.hpp"
#include "ui/anim.hpp"
#include "ui/focus.hpp"
#include "ui/hint_bar.hpp"
#include "ui/input.hpp"
#include "ui/modal.hpp"
#include "ui/screen.hpp"
#include "ui/toast.hpp"

namespace karafin::ui {

class ScrollView;

class Root final : public AppDelegate {
public:
    using ScreenFactory = std::function<std::unique_ptr<Screen>()>;

    explicit Root(ScreenFactory initialScreen);
    ~Root() override;

    static Root& instance();
    static bool exists();

    Navigator& nav() { return nav_; }
    FocusManager& focus() { return focus_; }
    Timeline& timeline() { return timeline_; }
    ToastHost& toasts() { return toasts_; }
    gfx::Renderer& renderer() { return renderer_; }

    void present(std::unique_ptr<Modal> modal);
    Modal* topModal() const;
    // Current focus scope: top modal (not dismissing) or top screen.
    View* focusScope() const;

    double time() const { return time_; }
    const DisplayInfo& display() const { return display_; }
    float scale() const { return display_.scale; }
    // True after touch input, false after controller input (widgets may soften focus visuals).
    bool touchMode() const { return touchMode_; }
    // Asks the user to confirm quitting (what an unhandled + does).
    void confirmQuit();
    void setShowStats(bool v) { showStats_ = v; }

    // AppDelegate
    void onStart() override;
    bool onEvent(const SDL_Event& e) override;
    void onUpdate(double dt) override;
    void onRender() override;
    void onDisplayChanged(const DisplayInfo& display) override;
    void onFocusChanged(bool focused) override;
    void onStop() override;

private:
    void dispatch(const InputEvent& e);
    void handleTouch(const SDL_Event& e);
    void updateScope();
    void collectHints(std::vector<Hint>& out) const;
    View* layerRoot() const;
    void renderStats(Renderer& r);

    // Performance overlay (ui.show_stats): aggregates since the last text refresh.
    struct StatsWindow {
        int frames = 0;
        double maxFrameMs = 0, cpuMs = 0, maxCpuMs = 0, swapMs = 0, maxUploadMs = 0, maxLayoutMs = 0;
        int uploads = 0, layouts = 0, glyphs = 0, slow = 0;
        size_t uploadBytes = 0;
    };
    StatsWindow statsWindow_;
    std::string statsLines_[3];
    double statsNext_ = 0;

    ScreenFactory initial_;
    gfx::Renderer renderer_;
    Navigator nav_;
    FocusManager focus_;
    Timeline timeline_;
    ToastHost toasts_;
    HintBar hintBar_;
    InputMapper input_;
    std::vector<std::unique_ptr<Modal>> modals_;
    std::vector<InputEvent> dispatching_;
    std::vector<Hint> hints_;
    DisplayInfo display_;
    double time_ = 0;
    float hintVisibility_ = 1.0f;
    bool touchMode_ = false;
    bool showStats_ = false;
    bool ready_ = false;
    bool quitDialogOpen_ = false;

    // Touch tracking (single finger).
    struct TouchSample {
        double t;
        Vec2 p;
    };
    bool touching_ = false;
    int64_t fingerId_ = -1;
    Vec2 touchStart_;
    Vec2 touchLast_;
    double touchStartTime_ = 0;
    Ref<View> touchTarget_;
    Ref<ScrollView> dragScroll_;
    std::vector<TouchSample> samples_;
};

// Shorthands (valid while the Root exists).
Root& root();
Navigator& nav();
Timeline& timeline();
FocusManager& focus();
void present(std::unique_ptr<Modal> modal);
void toast(std::string text, ToastKind kind = ToastKind::Info, gfx::Icon icon = gfx::Icon::None, float duration = 2.8f);
// Display scale (1.0 handheld, 1.5 docked) for sizing image requests in framebuffer pixels.
float displayScale();
// Seconds since the UI started (monotonic, frame time).
double now();

}  // namespace karafin::ui
