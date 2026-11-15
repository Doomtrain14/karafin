#include "ui/widgets/dialog.hpp"

#include <algorithm>
#include <limits>

#include "ui/root.hpp"

namespace karafin::ui {
namespace {
constexpr float kCardWidth = 520;
constexpr float kCardPad = 32;
}  // namespace

Dialog::Dialog(DialogSpec spec) : spec_(std::move(spec)) {
    setDim(0.62f);
    setDismissOnBackdropTap(spec_.cancelIndex >= 0 || spec_.buttons.empty());
    card_ = add<Box>(Axis::Vertical);
    card_->setPadding(Insets::all(kCardPad)).setSpacing(12).setAlign(Align::Center);
    if (spec_.icon != gfx::Icon::None) {
        auto* icon = card_->add<IconView>(spec_.icon, spec_.iconColor, 40);
        icon->setMargin({0, 0, 0, 4});
    }
    auto* title = card_->add<Label>(spec_.title, theme::type::title2);
    title->setAlign(gfx::TextAlign::Center);
    title->setMaxLines(2);
    if (!spec_.message.empty()) {
        auto* msg = card_->add<Label>(spec_.message, theme::type::body, theme::color::textSecondary);
        msg->setAlign(gfx::TextAlign::Center);
        msg->setMaxLines(8);
    }
    auto* row = card_->add<Box>(Axis::Horizontal);
    row->setSpacing(14).setAlign(Align::Center);
    row->setMargin({0, 14, 0, 0});
    for (size_t i = 0; i < spec_.buttons.size(); ++i) {
        const DialogButton& b = spec_.buttons[i];
        auto* btn = row->add<Button>(b.label, gfx::Icon::None, b.style);
        btn->setPreferredSize(std::max(140.0f, btn->measure({1000, 100}).x), theme::button::height);
        btn->onActivate = [this, i] { press(i); };
        buttons_.push_back(btn);
    }
    // Long labels: share the card width instead of overflowing it (labels ellipsize).
    if (!buttons_.empty()) {
        const float avail = kCardWidth - kCardPad * 2;
        const float spacing = 14.0f * static_cast<float>(buttons_.size() - 1);
        float total = spacing;
        for (Button* b : buttons_) total += b->preferredSize().x;
        if (total > avail) {
            const float each = (avail - spacing) / static_cast<float>(buttons_.size());
            for (Button* b : buttons_) b->setPreferredSize(each, theme::button::height);
        }
    }
    if (!buttons_.empty()) {
        const size_t def = static_cast<size_t>(std::clamp(spec_.defaultIndex, 0, static_cast<int>(buttons_.size()) - 1));
        setRememberedFocus(buttons_[def]);
    }
}

std::unique_ptr<Dialog> Dialog::confirm(std::string title, std::string message, std::string confirmLabel,
                                        std::string cancelLabel, std::function<void(bool)> onResult) {
    DialogSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    auto shared = std::make_shared<std::function<void(bool)>>(std::move(onResult));
    spec.buttons.push_back({std::move(cancelLabel), ButtonStyle::Secondary, [shared] {
                                if (*shared) (*shared)(false);
                            }});
    spec.buttons.push_back({std::move(confirmLabel), ButtonStyle::Primary, [shared] {
                                if (*shared) (*shared)(true);
                            }});
    spec.cancelIndex = 0;
    spec.defaultIndex = 1;
    return std::make_unique<Dialog>(std::move(spec));
}

std::unique_ptr<Dialog> Dialog::alert(std::string title, std::string message, std::string okLabel,
                                      std::function<void()> onClose) {
    DialogSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    spec.icon = gfx::Icon::Info;
    spec.buttons.push_back({std::move(okLabel), ButtonStyle::Primary, std::move(onClose)});
    spec.cancelIndex = 0;
    return std::make_unique<Dialog>(std::move(spec));
}

void Dialog::press(size_t index) {
    if (resolved_ || index >= spec_.buttons.size()) return;
    resolved_ = true;
    auto fn = spec_.buttons[index].onPress;
    dismiss();
    if (fn) fn();
}

bool Dialog::onInput(const InputEvent& e) {
    if (e.button == Key::B && e.pressed()) {
        if (spec_.cancelIndex >= 0) press(static_cast<size_t>(spec_.cancelIndex));
        else if (!resolved_) {
            resolved_ = true;
            dismiss();
        }
        return true;
    }
    return Modal::onInput(e);
}

void Dialog::layout() {
    const Vec2 m = card_->measure({kCardWidth, std::numeric_limits<float>::infinity()});
    const float h = m.y;
    card_->setFrame({(width() - kCardWidth) * 0.5f, (height() - h) * 0.5f, kCardWidth, h});
}

void Dialog::update(float dt) {
    Modal::update(dt);
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    card_->alpha.set(a);
    card_->scale.set(0.92f + 0.08f * appear());
    card_->offset.set({0, (1.0f - a) * 16.0f});
}

void Dialog::draw(Renderer& r) {
    const Rect c = card_->frame();
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    const float s = 0.92f + 0.08f * appear();
    r.pushTransform(gfx::Affine::translate(0, (1.0f - a) * 16.0f) * gfx::Affine::scaleAround(s, c.center()));
    r.pushAlpha(a);
    r.drawShadow(c, theme::radius::xl, 60, Color::black(0.65f), {0, 24});
    r.fillRoundRect(c, theme::radius::xl, gfx::Gradient::vertical(Color::hex(0x23232F), Color::hex(0x1A1A24)));
    r.strokeRoundRect(c, theme::radius::xl, 1, theme::color::stroke);
    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::ui
