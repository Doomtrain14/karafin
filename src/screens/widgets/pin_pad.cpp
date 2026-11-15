#include "screens/widgets/pin_pad.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "gfx/icons.hpp"
#include "gfx/text.hpp"
#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kCardW = 560;
constexpr float kCardH = 500;
constexpr float kDot = 22;
constexpr float kDotGap = 22;
constexpr float kDotsY = 200;     // from the card top
constexpr float kPadY = 322;      // center of the arrow cross, from the card top
constexpr float kArrow = 56;
constexpr float kArrowStep = 62;  // arrow center to the cross center
constexpr float kSideW = 104;
constexpr float kSideH = 44;
constexpr float kShakeTime = 0.45f;
constexpr float kFlashTime = 0.3f;
constexpr float kPressTime = 0.2f;

char stepFor(Key k) {
    switch (k) {
        case Key::Up: return 'U';
        case Key::Down: return 'D';
        case Key::Left: return 'L';
        case Key::Right: return 'R';
        default: return 0;
    }
}

int buttonFor(char step) {
    switch (step) {
        case 'U': return 0;
        case 'D': return 1;
        case 'L': return 2;
        case 'R': return 3;
        default: return -1;
    }
}

gfx::Icon chevronFor(char step) {
    switch (step) {
        case 'U': return gfx::Icon::ChevronUp;
        case 'D': return gfx::Icon::ChevronDown;
        case 'L': return gfx::Icon::ChevronLeft;
        default: return gfx::Icon::ChevronRight;
    }
}
}  // namespace

PinPad::PinPad(PinPadSpec spec) : spec_(std::move(spec)) {
    setDim(0.62f);
    setDismissOnBack(false);
    setDismissOnBackdropTap(false);
    if (spec_.lockout) lockLeft_ = static_cast<float>(std::max(0, spec_.lockout()));
}

Rect PinPad::card() const { return {(width() - kCardW) * 0.5f, (height() - kCardH) * 0.5f, kCardW, kCardH}; }

Rect PinPad::buttonRect(int button) const {
    const Rect c = card();
    const Vec2 o{c.center().x, c.y + kPadY};
    auto arrow = [&](float dx, float dy) {
        return Rect{o.x + dx - kArrow * 0.5f, o.y + dy - kArrow * 0.5f, kArrow, kArrow};
    };
    switch (button) {
        case Up: return arrow(0, -kArrowStep);
        case Down: return arrow(0, kArrowStep);
        case Left: return arrow(-kArrowStep, 0);
        case Right: return arrow(kArrowStep, 0);
        case Delete: return {o.x + 120, o.y + kArrowStep - kSideH * 0.5f, kSideW, kSideH};
        case Ok: return {o.x - 120 - kSideW, o.y + kArrowStep - kSideH * 0.5f, kSideW, kSideH};
        default: return {};
    }
}

void PinPad::submit() {
    std::string error;
    if (spec_.onSubmit && !spec_.onSubmit(code_, error)) {
        error_ = error;
        code_.clear();
        flash_ = 0;
        shake_ = kShakeTime;
        if (spec_.lockout) lockLeft_ = static_cast<float>(std::max(0, spec_.lockout()));
        return;
    }
    done_ = true;
    dismiss();
}

void PinPad::cancel() {
    done_ = true;
    dismiss();
    if (spec_.onCancel) spec_.onCancel();
}

void PinPad::addStep(char step) {
    if (lockedOut() || static_cast<int>(code_.size()) >= slots()) return;
    code_.push_back(step);
    error_.clear();
    pop_ = 1;
    flash_ = kFlashTime;
    pressed_ = buttonFor(step);
    pressT_ = kPressTime;
    if (static_cast<int>(code_.size()) == slots()) submit();
}

void PinPad::deleteStep() {
    if (code_.empty()) return;
    code_.pop_back();
    flash_ = 0;
}

void PinPad::press(int button) {
    if (button == Delete) {
        pressed_ = button;
        pressT_ = kPressTime;
        deleteStep();
        return;
    }
    if (button == Ok) {
        if (lockedOut() || !canSubmitShort()) return;
        pressed_ = button;
        pressT_ = kPressTime;
        submit();
        return;
    }
    static constexpr char steps[] = {'U', 'D', 'L', 'R'};
    if (button >= 0 && button < 4) addStep(steps[button]);
}

bool PinPad::onInput(const InputEvent& e) {
    if (done_ || dismissing()) return true;
    if (!e.pressed()) return true;
    if (const char step = stepFor(e.button)) {
        addStep(step);
        return true;
    }
    if (e.button == Key::A) {
        if (!lockedOut() && canSubmitShort()) submit();
        return true;
    }
    if (e.button == Key::B) {
        if (code_.empty()) cancel();
        else deleteStep();
        return true;
    }
    if (e.button == Key::Y) {
        code_.clear();
        flash_ = 0;
        return true;
    }
    return e.button != Key::Plus;
}

bool PinPad::onTap(Vec2 local) {
    if (done_ || dismissing()) return true;
    for (int b = 0; b < ButtonCount; ++b) {
        if (b == Ok && spec_.length != 0) continue;
        if (buttonRect(b).contains(local)) {
            press(b);
            break;
        }
    }
    return true;
}

void PinPad::update(float dt) {
    Modal::update(dt);
    shake_ = std::max(0.0f, shake_ - dt);
    pop_ = std::max(0.0f, pop_ - dt * 6);
    flash_ = std::max(0.0f, flash_ - dt);
    pressT_ = std::max(0.0f, pressT_ - dt);
    if (lockLeft_ > 0) {
        lockLeft_ = std::max(0.0f, lockLeft_ - dt);
        if (lockLeft_ == 0) error_.clear();
    }
}

void PinPad::draw(Renderer& r) {
    using namespace theme;
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    const float s = 0.92f + 0.08f * appear();
    const Rect c = card();
    const float shakeX = shake_ > 0 ? std::sin(shake_ * 60.0f) * 12.0f * (shake_ / kShakeTime) : 0.0f;
    r.pushTransform(gfx::Affine::translate(shakeX, (1.0f - a) * 16.0f) * gfx::Affine::scaleAround(s, c.center()));
    r.pushAlpha(a);
    r.drawShadow(c, radius::xl, 60, Color::black(0.65f), {0, 24});
    r.fillRoundRect(c, radius::xl, gfx::Gradient::vertical(Color::hex(0x23232F), Color::hex(0x1A1A24)));
    r.strokeRoundRect(c, radius::xl, 1, color::stroke);

    const float cx = c.center().x;
    float y = c.y + 28;
    const bool locked = lockedOut();
    gfx::drawIcon(r, gfx::Icon::Lock, {cx - 18, y, 36, 36}, locked ? color::danger : color::accent, 1.6f);
    y += 48;
    const float textW = kCardW - 64;
    auto title = gfx::text::layout(spec_.title, type::title2, {.maxWidth = textW, .align = gfx::TextAlign::Center});
    r.drawText(*title, {cx - title->width * 0.5f, y}, color::textPrimary);
    y += title->height + 8;
    std::string message = spec_.message;
    if (locked)
        message = std::format("Too many tries. Try again in {} s", static_cast<int>(std::ceil(lockLeft_)));
    else if (!error_.empty())
        message = error_;
    auto msg = gfx::text::layout(message, type::body, {.maxWidth = textW, .maxLines = 2, .align = gfx::TextAlign::Center});
    r.drawText(*msg, {cx - msg->width * 0.5f, y}, locked || !error_.empty() ? color::danger : color::textSecondary);

    const int n = slots();
    const int entered = static_cast<int>(code_.size());
    const float rowW = n * kDot + (n - 1) * kDotGap;
    const float dotY = c.y + kDotsY;
    const float flash = flash_ / kFlashTime;
    for (int i = 0; i < n; ++i) {
        const Vec2 p{cx - rowW * 0.5f + i * (kDot + kDotGap) + kDot * 0.5f, dotY};
        // With a 4-or-6 code the last two slots are optional.
        const bool optional = spec_.length == 0 && i >= 4;
        if (i < entered) {
            const bool newest = i == entered - 1;
            const float grow = newest ? 1.0f + 0.35f * pop_ : 1.0f;
            if (newest && flash > 0) {
                const float is = kDot + 8;
                gfx::drawIcon(r, chevronFor(code_.back()), {p.x - is * 0.5f, p.y - is * 0.5f, is, is},
                              color::accentBright.mulAlpha(flash), 1.8f);
                r.fillCircle(p, kDot * 0.5f * grow, color::accent.mulAlpha(1.0f - flash));
            } else {
                r.fillCircle(p, kDot * 0.5f * grow, color::accent);
            }
        } else {
            r.strokeRoundRect({p.x - kDot * 0.5f, p.y - kDot * 0.5f, kDot, kDot}, kDot * 0.5f, 2,
                              optional ? color::textTertiary.mulAlpha(0.6f) : color::textSecondary);
        }
    }

    const bool full = entered >= n;
    for (int b = 0; b < ButtonCount; ++b) {
        if (b == Ok && spec_.length != 0) continue;
        const Rect br = buttonRect(b);
        const bool enabled = b == Delete ? !code_.empty() : b == Ok ? canSubmitShort() && !locked : !locked && !full;
        const float press = pressed_ == b ? pressT_ / kPressTime : 0.0f;
        const float rad = b == Delete || b == Ok ? br.h * 0.5f : radius::lg;
        Color fill = color::buttonSecondary;
        if (b == Ok && enabled) fill = color::accent;
        r.fillRoundRect(br, rad, fill.mulAlpha(enabled ? 1.0f : 0.5f));
        if (press > 0) r.fillRoundRect(br, rad, Color::white(0.28f * press));
        r.strokeRoundRect(br, rad, 1, color::stroke);
        const Color ink = enabled ? color::textPrimary : color::textTertiary;
        if (b < 4) {
            static constexpr gfx::Icon icons[] = {gfx::Icon::ChevronUp, gfx::Icon::ChevronDown, gfx::Icon::ChevronLeft,
                                                  gfx::Icon::ChevronRight};
            gfx::drawIcon(r, icons[b], {br.center().x - 14, br.center().y - 14, 28, 28}, ink, 1.4f);
        } else {
            const gfx::Icon icon = b == Delete ? gfx::Icon::Back : gfx::Icon::Check;
            auto label = gfx::text::layout(b == Delete ? "Delete" : "OK", type::button);
            const float w = 18 + 6 + label->width;
            const float x = br.center().x - w * 0.5f;
            gfx::drawIcon(r, icon, {x, br.center().y - 9, 18, 18}, ink, 1.2f);
            r.drawText(*label, {x + 24, br.center().y - label->height * 0.5f}, ink);
        }
    }

    std::string help;
    if (locked)
        help = "B  Cancel";
    else if (spec_.length == 0)
        help = code_.size() == 4 ? "A  Use 4 steps     B  Delete" : "D-pad or arrows: 4 or 6 steps     B  Delete";
    else
        help = code_.empty() ? "D-pad or arrows     B  Cancel" : "B  Delete";
    auto hint = gfx::text::layout(help, type::footnote, {.maxWidth = textW, .align = gfx::TextAlign::Center});
    r.drawText(*hint, {cx - hint->width * 0.5f, c.bottom() - 40}, color::textTertiary);

    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::screens
