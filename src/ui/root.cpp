#include "ui/root.hpp"

#include <algorithm>
#include <format>

#include "core/log.hpp"
#include "core/perf.hpp"
#include "core/settings.hpp"
#include "gfx/image.hpp"
#include "ui/scroll.hpp"
#include "ui/sound.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::ui {
namespace {
Root* g_root = nullptr;
constexpr float kTapSlop = 14.0f;
constexpr float kDragSlop = 12.0f;
constexpr size_t kMaxTouchSamples = 32;
}  // namespace

Root::Root(ScreenFactory initialScreen) : initial_(std::move(initialScreen)) { g_root = this; }

Root::~Root() {
    if (g_root == this) g_root = nullptr;
}

Root& Root::instance() { return *g_root; }
bool Root::exists() { return g_root != nullptr; }

void Root::onStart() {
    display_ = App::instance().display();
    if (!renderer_.init()) log::error("ui", "renderer init failed");
    gfx::ImageCache::instance().init();
    showStats_ = Settings::instance().get<bool>("ui.show_stats", false);
    dispatching_.reserve(16);
    hints_.reserve(12);
    samples_.reserve(kMaxTouchSamples);
    int controllers = 0;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) ++controllers;
        else SDL_JoystickOpen(i);
    }
    log::info("ui", "{} joysticks ({} game controllers)", SDL_NumJoysticks(), controllers);
    nav_.layout({display_.logicalWidth, display_.logicalHeight});
    if (initial_) nav_.push(initial_(), Transition::None);
    updateScope();
    ready_ = true;
    log::info("ui", "root started ({}x{} @{:.2f})", display_.pixelWidth, display_.pixelHeight, display_.scale);
}

void Root::onStop() {
    ready_ = false;
    sound::shutdown();
    modals_.clear();
    nav_.clear();
    focus_.clear();
    timeline_.clear();
    toasts_.clear();
    gfx::ImageCache::instance().shutdown();
    renderer_.shutdown();
}

namespace {
void markLayoutDirty(View* v) {
    v->setNeedsLayout();
    for (const auto& c : v->children()) markLayoutDirty(c.get());
}
}  // namespace

void Root::onDisplayChanged(const DisplayInfo& display) {
    // App reports the same mode twice (its own SDL_SetWindowSize triggers a SIZE_CHANGED event).
    const bool scaleChanged = display.scale != display_.scale;
    if (display.pixelWidth == display_.pixelWidth && display.pixelHeight == display_.pixelHeight && !scaleChanged &&
        display.logicalWidth == display_.logicalWidth && display.logicalHeight == display_.logicalHeight)
        return;
    display_ = display;
    if (scaleChanged) {
        // Text metrics are hinted at the new pixel size: switch text now (not at the next
        // beginFrame) and re-measure everything, so single-line labels sized at the old scale do
        // not truncate.
        gfx::text::setDisplayScale(display.scale);
        for (size_t i = 0; i < nav_.depth(); ++i) markLayoutDirty(nav_.at(i));
        for (auto& m : modals_) markLayoutDirty(m.get());
    }
    nav_.layout({display.logicalWidth, display.logicalHeight});
    log::info("ui", "display changed: {}x{} @{:.2f}", display.pixelWidth, display.pixelHeight, display.scale);
}

void Root::onFocusChanged(bool focused) {
    if (!focused) {
        sound::stop();
        input_.reset(&dispatching_);
        for (const auto& e : dispatching_) dispatch(e);
        dispatching_.clear();
        if (touching_) {
            if (View* target = touchTarget_.get()) target->onTouchCancel();
            if (ScrollView* sv = dragScroll_.get()) sv->dragEnd(0);
            touching_ = false;
            touchTarget_.reset();
            dragScroll_.reset();
        }
    }
}

bool Root::onEvent(const SDL_Event& e) {
    if (!ready_) return false;
    switch (e.type) {
        case SDL_JOYDEVICEADDED:
            // Joysticks without a game-controller mapping are read through raw SDL_JOY* events.
            if (!SDL_IsGameController(e.jdevice.which)) SDL_JoystickOpen(e.jdevice.which);
            return false;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            if (!topModal())
                if (Screen* screen = nav_.top(); screen && screen->onTouchEvent(e)) {
                    touchMode_ = true;
                    return true;
                }
            handleTouch(e);
            return true;
        default: {
            if (!input_.handle(e, dispatching_)) return false;
            for (const auto& ev : dispatching_) dispatch(ev);
            dispatching_.clear();
            return true;
        }
    }
}

Modal* Root::topModal() const {
    for (auto it = modals_.rbegin(); it != modals_.rend(); ++it)
        if (!(*it)->dismissing()) return it->get();
    return nullptr;
}

View* Root::focusScope() const {
    if (Modal* m = topModal()) return m;
    return nav_.top();
}

View* Root::layerRoot() const { return focusScope(); }

void Root::updateScope() { focus_.setScope(focusScope()); }

void Root::present(std::unique_ptr<Modal> modal) {
    if (!modal) return;
    Modal* m = modal.get();
    m->setFrame({0, 0, display_.logicalWidth, display_.logicalHeight});
    modals_.push_back(std::move(modal));
    m->layoutIfNeeded();
    m->onPresented();
    updateScope();
}

void Root::confirmQuit() {
    if (quitDialogOpen_) return;
    quitDialogOpen_ = true;
    auto dialog = Dialog::confirm("Quit Karafin?", "Playback and downloads will stop.", "Quit", "Cancel",
                                  [](bool ok) {
                                      if (ok) App::instance().requestQuit();
                                  });
    dialog->onDismissed = [this] { quitDialogOpen_ = false; };
    present(std::move(dialog));
}

void Root::dispatch(const InputEvent& e) {
    if (e.action != InputAction::Release) touchMode_ = false;
    View* scope = focusScope();
    if (!scope) return;
    const auto* screen = dynamic_cast<Screen*>(scope);
    if ((!screen || screen->navigationSounds()) && e.pressedOrRepeat()) {
        if (navDirection(e.button) || e.button == Key::L || e.button == Key::R)
            sound::play(sound::Cue::Move);
        else if (e.pressed() && e.button == Key::A)
            sound::play(sound::Cue::Select);
        else if (e.pressed() && e.button == Key::B)
            sound::play(sound::Cue::Back);
    }
    View* f = focus_.focused();
    if (f && f->isDescendantOf(scope)) {
        for (View* v = f; v; v = v->parent()) {
            if (v->onInput(e)) return;
            if (v == scope) break;
        }
    } else if (scope->onInput(e)) {
        return;
    }
    if (e.action == InputAction::Release) return;

    if (auto dir = navDirection(e.button)) {
        focus_.navigate(*dir);
        return;
    }
    if (e.button == Key::B && e.pressed()) {
        Screen* s = nav_.top();
        if (s && !s->onBack() && nav_.depth() > 1) nav_.pop();
        return;
    }
    if (e.button == Key::Plus && e.pressed()) confirmQuit();
}

void Root::handleTouch(const SDL_Event& e) {
    const Vec2 p{e.tfinger.x * display_.logicalWidth, e.tfinger.y * display_.logicalHeight};
    const double t = static_cast<double>(e.tfinger.timestamp) / 1000.0;
    switch (e.type) {
        case SDL_FINGERDOWN: {
            if (touching_) return;
            touching_ = true;
            touchMode_ = true;
            fingerId_ = static_cast<int64_t>(e.tfinger.fingerId);
            touchStart_ = touchLast_ = p;
            touchStartTime_ = t;
            samples_.clear();
            samples_.push_back({t, p});
            View* layer = layerRoot();
            View* hit = layer ? layer->hitTestDeep(p) : nullptr;
            touchTarget_ = hit;
            dragScroll_.reset();
            // Touching a moving list stops it (and does not count as a tap).
            for (View* v = hit; v; v = v->parent()) {
                if (auto* sv = dynamic_cast<ScrollView*>(v); sv && sv->moving()) {
                    dragScroll_ = sv;
                    sv->dragBegin();
                    break;
                }
            }
            if (!dragScroll_ && hit) hit->onTouchDown();
            break;
        }
        case SDL_FINGERMOTION: {
            // The devkitPro port reports motion with the touch *slot index* instead of the finger
            // id (and SDL turns an unknown id into a phantom FINGERDOWN), so motion cannot be
            // matched by id. Single-finger UI: follow any motion while the tracked finger is down.
            if (!touching_) return;
            const Vec2 delta = p - touchLast_;
            touchLast_ = p;
            if (samples_.size() >= kMaxTouchSamples) samples_.erase(samples_.begin());
            samples_.push_back({t, p});
            while (samples_.size() > 2 && t - samples_.front().t > 0.12) samples_.erase(samples_.begin());
            if (!dragScroll_) {
                const Vec2 total = p - touchStart_;
                if (total.length() > kDragSlop) {
                    const bool horizontal = std::fabs(total.x) > std::fabs(total.y);
                    for (View* v = touchTarget_.get(); v; v = v->parent()) {
                        auto* sv = dynamic_cast<ScrollView*>(v);
                        if (sv && sv->canScroll() && (sv->axis() == Axis::Horizontal) == horizontal) {
                            dragScroll_ = sv;
                            sv->dragBegin();
                            break;
                        }
                    }
                    if (View* target = touchTarget_.get()) target->onTouchCancel();
                    if (!dragScroll_) touchTarget_.reset();  // moved too far for a tap
                }
            }
            if (ScrollView* sv = dragScroll_.get()) sv->dragMove(sv->axis() == Axis::Horizontal ? delta.x : delta.y);
            break;
        }
        case SDL_FINGERUP: {
            if (!touching_ || static_cast<int64_t>(e.tfinger.fingerId) != fingerId_) return;
            touching_ = false;
            if (ScrollView* sv = dragScroll_.get()) {
                Vec2 v{0, 0};
                const TouchSample* ref = nullptr;
                for (const auto& s : samples_)
                    if (t - s.t <= 0.1) {
                        ref = &s;
                        break;
                    }
                if (ref && t - ref->t > 0.005) v = (p - ref->p) / static_cast<float>(t - ref->t);
                sv->dragEnd(sv->axis() == Axis::Horizontal ? v.x : v.y);
            } else if (View* target = touchTarget_.get()) {
                if ((p - touchStart_).length() < kTapSlop && t - touchStartTime_ < 0.8) {
                    View* layer = layerRoot();
                    const auto* screen = dynamic_cast<Screen*>(layer);
                    const bool sounds = !screen || screen->navigationSounds();
                    for (View* v = target; v; v = v->parent()) {
                        if (v->onTap(v->toLocal(p))) {
                            if (sounds) sound::play(sound::Cue::Select);
                            break;
                        }
                        if (v == layer) break;
                    }
                } else {
                    target->onTouchCancel();
                }
            }
            touchTarget_.reset();
            dragScroll_.reset();
            break;
        }
        default: break;
    }
}

void Root::collectHints(std::vector<Hint>& out) const {
    out.clear();
    auto add = [&](const Hint& h) {
        if (h.label.empty()) return;
        for (const auto& x : out)
            if (x.button == h.button) return;
        out.push_back(h);
    };
    View* scope = focusScope();
    View* f = focus_.focused();
    if (f && scope && f->isDescendantOf(scope)) {
        for (View* v = f; v; v = v->parent()) {
            for (const auto& h : v->hints()) add(h);
            if (v == scope) break;
        }
    } else if (scope) {
        for (const auto& h : scope->hints()) add(h);
    }
    if (f && f->onActivate) add({Key::A, "Select"});
    if (topModal()) add({Key::B, "Close"});
    else if (nav_.depth() > 1) add({Key::B, "Back"});
    // Insertion sort: stable, allocation-free (std::stable_sort allocates a buffer), n is tiny.
    for (size_t i = 1; i < out.size(); ++i)
        for (size_t j = i; j > 0 && static_cast<int>(out[j].button) < static_cast<int>(out[j - 1].button); --j)
            std::swap(out[j], out[j - 1]);
}

void Root::onUpdate(double dtD) {
    if (!ready_) return;
    const auto dt = static_cast<float>(dtD);
    time_ += dtD;

    input_.update(dt, dispatching_);
    for (const auto& e : dispatching_) dispatch(e);
    dispatching_.clear();

    timeline_.update(dt);
    gfx::ImageCache::instance().update();

    // Cheap when nothing changed: frames are only re-set on size changes and layoutIfNeeded
    // skips clean subtrees.
    const Vec2 size{display_.logicalWidth, display_.logicalHeight};
    nav_.layout(size);
    for (auto& m : modals_) {
        m->setFrame({0, 0, size.x, size.y});
        m->layoutIfNeeded();
    }

    nav_.update(dt);
    for (size_t i = 0; i < modals_.size(); ++i) modals_[i]->update(dt);
    for (size_t i = 0; i < modals_.size();) {
        if (modals_[i]->finished()) {
            std::unique_ptr<Modal> m = std::move(modals_[i]);
            modals_.erase(modals_.begin() + static_cast<std::ptrdiff_t>(i));
            auto cb = std::move(m->onDismissed);
            m.reset();
            if (cb) cb();
        } else {
            ++i;
        }
    }
    toasts_.update(dt);

    updateScope();
    focus_.validate();

    collectHints(hints_);
    hintBar_.setHints(hints_);
    hintBar_.update(dt);
    const Screen* top = nav_.top();
    const float hintTarget = (top && top->showsHintBar()) ? 1.0f : 0.0f;
    hintVisibility_ += (hintTarget - hintVisibility_) * std::min(1.0f, dt * 10.0f);
}

void Root::onRender() {
    if (!ready_) return;
    gfx::Renderer& r = renderer_;
    r.beginFrame(App::instance().display());
    r.clear(theme::color::background);
    nav_.render(r);

    const Rect full{0, 0, display_.logicalWidth, display_.logicalHeight};
    for (auto& m : modals_) {
        const float a = std::clamp(m->appear(), 0.0f, 1.0f);
        if (m->dim() > 0 && a > 0) r.fillRect(full, Color::black(m->dim() * a));
        m->render(r);
    }

    hintBar_.render(r, full.w, full.h, hintVisibility_);
    toasts_.render(r, full.w, full.h - theme::layout::hintBarHeight - 14);

    if (showStats_) renderStats(r);
    r.endFrame();
}

void Root::renderStats(Renderer& r) {
    // Accumulate the previous (completed) frame; refresh the text 4x per second so the overlay
    // itself does not shape new text every frame.
    const perf::Frame& f = perf::last();
    StatsWindow& w = statsWindow_;
    w.frames++;
    w.maxFrameMs = std::max(w.maxFrameMs, f.frameMs);
    w.cpuMs += f.cpuMs;
    w.maxCpuMs = std::max(w.maxCpuMs, f.cpuMs);
    w.swapMs += f.swapMs;
    w.uploads += f.uploads;
    w.uploadBytes += f.uploadBytes;
    w.maxUploadMs = std::max(w.maxUploadMs, f.uploadMs);
    w.layouts += f.layouts;
    w.maxLayoutMs = std::max(w.maxLayoutMs, f.layoutMs);
    w.glyphs += f.glyphs;
    w.slow += f.frameMs > 25.0 ? 1 : 0;
    if (time_ >= statsNext_ || statsLines_[0].empty()) {
        statsNext_ = time_ + 0.25;
        const double n = std::max(1, w.frames);
        const double gpu = perf::gpuMs();
        statsLines_[0] = std::format("{:.0f} fps  frame max {:.1f} ms  cpu {:.1f} (max {:.1f})  gpu {}  swap {:.1f}  slow {}",
                                     App::instance().fps(), w.maxFrameMs, w.cpuMs / n, w.maxCpuMs,
                                     gpu >= 0 ? std::format("{:.1f}", gpu) : std::string("n/a"), w.swapMs / n, w.slow);
        statsLines_[1] = std::format("upload {} img {} KiB (max {:.1f} ms)  text {} layouts (max {:.1f} ms) {} glyphs",
                                     w.uploads, w.uploadBytes / 1024, w.maxUploadMs, w.layouts, w.maxLayoutMs, w.glyphs);
        statsLines_[2] = std::format("{} draws  {} quads  {} rt  img {} MiB / {}  queue {}", f.drawCalls, f.quads,
                                     f.targets, gfx::ImageCache::instance().memoryUsed() >> 20,
                                     gfx::ImageCache::instance().entryCount(),
                                     gfx::ImageCache::instance().pendingUploads());
        w = {};
    }

    constexpr float kX = 8, kY = 8, kPad = 8, kGraphH = 50, kMsToPx = 1.5f;
    float textW = 0, textH = 0;
    gfx::TextLayoutPtr layouts[3];
    for (int i = 0; i < 3; ++i) {
        layouts[i] = gfx::text::layout(statsLines_[i], theme::type::footnote);
        textW = std::max(textW, layouts[i]->width);
        textH += layouts[i]->height;
    }
    const auto& hist = perf::history();
    const float barW = 2.0f;
    const float graphW = barW * static_cast<float>(perf::kHistory);
    const float boxW = std::max(textW, graphW) + kPad * 2;
    r.fillRoundRect({kX, kY, boxW, textH + kGraphH + kPad * 3}, 6, Color::black(0.65f));
    float y = kY + kPad;
    for (const auto& l : layouts) {
        r.drawText(*l, {kX + kPad, y}, theme::color::success);
        y += l->height;
    }
    // Frame-time graph, oldest left. Bars: green within one vsync, amber within two, red beyond;
    // the white inner bar is the CPU part of the frame.
    const float base = y + kPad + kGraphH;
    for (size_t i = 0; i < perf::kHistory; ++i) {
        const perf::Sample& s = hist[(perf::historyHead() + i) % perf::kHistory];
        if (s.frameMs <= 0) continue;
        const float x = kX + kPad + static_cast<float>(i) * barW;
        const float h = std::min(kGraphH, s.frameMs * kMsToPx);
        const Color c = s.frameMs <= 18.0f ? theme::color::success
                        : s.frameMs <= 34.5f ? theme::color::warning
                                             : theme::color::danger;
        r.fillRect({x, base - h, barW - 0.5f, h}, c.withAlpha(0.8f));
        const float ch = std::min(kGraphH, s.cpuMs * kMsToPx);
        r.fillRect({x, base - ch, barW - 0.5f, ch}, Color::white(0.45f));
    }
    r.fillRect({kX + kPad, base - 16.67f * kMsToPx, graphW, 1}, Color::white(0.6f));
}

// ---------------------------------------------------------------------------------------------

Root& root() { return Root::instance(); }
Navigator& nav() { return Root::instance().nav(); }
Timeline& timeline() { return Root::instance().timeline(); }
FocusManager& focus() { return Root::instance().focus(); }
void present(std::unique_ptr<Modal> modal) {
    if (Root::exists()) Root::instance().present(std::move(modal));
}
void toast(std::string text, ToastKind kind, gfx::Icon icon, float duration) {
    if (Root::exists()) Root::instance().toasts().show(std::move(text), kind, icon, duration);
}
float displayScale() { return Root::exists() ? Root::instance().scale() : 1.0f; }
double now() { return Root::exists() ? Root::instance().time() : 0.0; }

}  // namespace karafin::ui
