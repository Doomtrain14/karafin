#include "ui/modal.hpp"

#include "ui/theme.hpp"

namespace karafin::ui {

Modal::Modal() {
    setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    setRememberFocus(true);
    appear_.springTo(1.0f, theme::motion::standard);
}

void Modal::dismiss() {
    if (dismissing_) return;
    dismissing_ = true;
    appear_.springTo(0.0f, {0.28f, 1.0f});
}

bool Modal::onInput(const InputEvent& e) {
    if (e.button == Key::B && e.pressed() && dismissOnBack_) {
        dismiss();
        return true;
    }
    if (View::onInput(e)) return true;
    // Swallow everything except directions (focus navigation) so nothing leaks to the screen
    // underneath; + still reaches the root (quit).
    return !navDirection(e.button) && e.button != Key::Plus;
}

bool Modal::onTap(Vec2 local) {
    // Taps on content panels are consumed; taps on the backdrop dismiss.
    for (const auto& c : children())
        if (c->visible() && c->frame().contains(local)) return true;
    if (dismissOnBackdropTap_) dismiss();
    return true;
}

void Modal::update(float dt) {
    appear_.update(dt);
    View::update(dt);
}

}  // namespace karafin::ui
