#include "screens/widgets/busy_modal.hpp"

#include <algorithm>

#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kCardW = 420;
constexpr float kCardH = 168;
}  // namespace

BusyModal::BusyModal(std::string message, std::function<void()> onCancel) : onCancel_(std::move(onCancel)) {
    setDim(0.55f);
    setDismissOnBack(false);
    setDismissOnBackdropTap(false);
    spinner_ = add<Spinner>(40, theme::color::accentBright);
    label_ = add<Label>(std::move(message), theme::type::headline);
    label_->setAlign(gfx::TextAlign::Center);
    label_->setMaxLines(2);
    hint_ = add<Label>(onCancel_ ? "Press B to cancel" : "", theme::type::footnote, theme::color::textTertiary);
    hint_->setAlign(gfx::TextAlign::Center);
}

void BusyModal::finish() {
    onCancel_ = nullptr;
    if (!dismissing()) dismiss();
}

bool BusyModal::onInput(const InputEvent& e) {
    if (e.button == Key::B && e.pressed()) {
        if (onCancel_) {
            auto fn = std::move(onCancel_);
            onCancel_ = nullptr;
            dismiss();
            fn();
        }
        return true;
    }
    // Swallow everything else: nothing behind the card may react while busy.
    return true;
}

void BusyModal::layout() {
    card_ = {(width() - kCardW) * 0.5f, (height() - kCardH) * 0.5f, kCardW, kCardH};
    spinner_->setFrame({card_.center().x - 20, card_.y + 28, 40, 40});
    label_->setFrame({card_.x + 24, card_.y + 84, kCardW - 48, 44});
    hint_->setFrame({card_.x + 24, card_.bottom() - 30, kCardW - 48, 18});
}

void BusyModal::update(float dt) {
    Modal::update(dt);
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    for (View* v : {static_cast<View*>(spinner_), static_cast<View*>(label_), static_cast<View*>(hint_)}) v->alpha.set(a);
}

void BusyModal::draw(Renderer& r) {
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    const float s = 0.94f + 0.06f * a;
    r.pushTransform(gfx::Affine::scaleAround(s, card_.center()));
    r.pushAlpha(a);
    r.drawShadow(card_, theme::radius::xl, 50, Color::black(0.6f), {0, 20});
    r.fillRoundRect(card_, theme::radius::xl, gfx::Gradient::vertical(Color::hex(0x23232F), Color::hex(0x1A1A24)));
    r.strokeRoundRect(card_, theme::radius::xl, 1, theme::color::stroke);
    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::screens
