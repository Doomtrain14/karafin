#include "screens/widgets/pin_pad.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/icons.hpp"
#include "gfx/text.hpp"
#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kCardW = 520;
constexpr float kCardH = 330;
constexpr float kDot = 22;
constexpr float kDotGap = 22;
constexpr float kShakeTime = 0.45f;

char stepFor(Key k) {
    switch (k) {
        case Key::Up: return 'U';
        case Key::Down: return 'D';
        case Key::Left: return 'L';
        case Key::Right: return 'R';
        default: return 0;
    }
}
}  // namespace

PinPad::PinPad(PinPadSpec spec) : spec_(std::move(spec)) {
    setDim(0.62f);
    setDismissOnBack(false);
    setDismissOnBackdropTap(false);
}

void PinPad::submit() {
    std::string error;
    if (spec_.onSubmit && !spec_.onSubmit(code_, error)) {
        error_ = error;
        code_.clear();
        shake_ = kShakeTime;
        return;
    }
    done_ = true;
    dismiss();
}

bool PinPad::onInput(const InputEvent& e) {
    if (done_ || dismissing()) return true;
    if (!e.pressed()) return true;
    if (const char step = stepFor(e.button)) {
        if (static_cast<int>(code_.size()) >= slots()) return true;
        code_.push_back(step);
        error_.clear();
        pop_ = 1;
        if (static_cast<int>(code_.size()) == slots()) submit();
        return true;
    }
    if (e.button == Key::A) {
        if (spec_.length == 0 && code_.size() == 4) submit();
        return true;
    }
    if (e.button == Key::B) {
        if (code_.empty()) {
            done_ = true;
            dismiss();
        } else {
            code_.pop_back();
        }
        return true;
    }
    if (e.button == Key::Y) {
        code_.clear();
        return true;
    }
    return e.button != Key::Plus;
}

void PinPad::update(float dt) {
    Modal::update(dt);
    shake_ = std::max(0.0f, shake_ - dt);
    pop_ = std::max(0.0f, pop_ - dt * 6);
}

void PinPad::draw(Renderer& r) {
    using namespace theme;
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    const float s = 0.92f + 0.08f * appear();
    const Rect c{(width() - kCardW) * 0.5f, (height() - kCardH) * 0.5f, kCardW, kCardH};
    const float shakeX = shake_ > 0 ? std::sin(shake_ * 60.0f) * 12.0f * (shake_ / kShakeTime) : 0.0f;
    r.pushTransform(gfx::Affine::translate(shakeX, (1.0f - a) * 16.0f) * gfx::Affine::scaleAround(s, c.center()));
    r.pushAlpha(a);
    r.drawShadow(c, radius::xl, 60, Color::black(0.65f), {0, 24});
    r.fillRoundRect(c, radius::xl, gfx::Gradient::vertical(Color::hex(0x23232F), Color::hex(0x1A1A24)));
    r.strokeRoundRect(c, radius::xl, 1, color::stroke);

    const float cx = c.center().x;
    float y = c.y + 30;
    gfx::drawIcon(r, gfx::Icon::Lock, {cx - 18, y, 36, 36}, color::accent, 1.6f);
    y += 50;
    const float textW = kCardW - 64;
    auto title = gfx::text::layout(spec_.title, type::title2, {.maxWidth = textW, .align = gfx::TextAlign::Center});
    r.drawText(*title, {cx - title->width * 0.5f, y}, color::textPrimary);
    y += title->height + 8;
    const bool showError = !error_.empty();
    auto msg = gfx::text::layout(showError ? error_ : spec_.message, type::body,
                                 {.maxWidth = textW, .maxLines = 2, .align = gfx::TextAlign::Center});
    r.drawText(*msg, {cx - msg->width * 0.5f, y}, showError ? color::danger : color::textSecondary);

    const int n = slots();
    const float rowW = n * kDot + (n - 1) * kDotGap;
    const float dotY = c.bottom() - 112;
    for (int i = 0; i < n; ++i) {
        const Vec2 p{cx - rowW * 0.5f + i * (kDot + kDotGap) + kDot * 0.5f, dotY};
        const bool filled = i < static_cast<int>(code_.size());
        // With a 4-or-6 code the last two slots are optional.
        const bool optional = spec_.length == 0 && i >= 4;
        if (filled) {
            const float grow = i == static_cast<int>(code_.size()) - 1 ? 1.0f + 0.35f * pop_ : 1.0f;
            r.fillCircle(p, kDot * 0.5f * grow, color::accent);
        } else {
            r.strokeRoundRect({p.x - kDot * 0.5f, p.y - kDot * 0.5f, kDot, kDot}, kDot * 0.5f, 2,
                              optional ? color::textTertiary.mulAlpha(0.6f) : color::textSecondary);
        }
    }

    std::string help;
    if (spec_.length == 0)
        help = code_.size() == 4 ? "A  Use 4 steps     B  Delete" : "Use the D-pad: 4 or 6 steps     B  Delete";
    else
        help = code_.empty() ? "Use the D-pad     B  Cancel" : "B  Delete";
    auto hint = gfx::text::layout(help, type::footnote, {.maxWidth = textW, .align = gfx::TextAlign::Center});
    r.drawText(*hint, {cx - hint->width * 0.5f, c.bottom() - 58}, color::textTertiary);

    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::screens
