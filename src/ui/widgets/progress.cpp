#include "ui/widgets/progress.hpp"

#include <algorithm>
#include <cmath>

namespace karafin::ui {

ProgressBar::ProgressBar() { setPreferredSize(-1, 6); }

void ProgressBar::setValue(float v, bool animated) {
    v = std::clamp(v, 0.0f, 1.0f);
    if (animated) value_.springTo(v, theme::motion::standard);
    else value_.set(v);
}

Vec2 ProgressBar::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    return {pref.x >= 0 ? pref.x : available.x, pref.y >= 0 ? pref.y : 6.0f};
}

void ProgressBar::update(float dt) {
    value_.update(dt);
    phase_ = std::fmod(phase_ + dt / 1.5f, 1.0f);
    View::update(dt);
}

void ProgressBar::draw(Renderer& r) {
    const Rect b = bounds();
    const float rad = b.h * 0.5f;
    r.fillRoundRect(b, rad, track_);
    if (indeterminate_) {
        const float segW = b.w * 0.3f;
        const float t = ease::inOutSine(phase_);
        const float x = -segW + (b.w + segW) * t;
        r.pushClip(b);
        r.fillRoundRect({x, 0, segW, b.h}, rad, fill_);
        r.popClip();
        return;
    }
    if (buffer_ > 0) r.fillRoundRect({0, 0, std::max(b.h, b.w * std::clamp(buffer_, 0.0f, 1.0f)), b.h}, rad, Color::white(0.18f));
    const float v = std::clamp(value_.value(), 0.0f, 1.0f);
    if (v <= 0) return;
    const Rect fill{0, 0, std::max(b.h, b.w * v), b.h};
    if (gradient_) r.fillRoundRect(fill, rad, theme::accentGradient);
    else r.fillRoundRect(fill, rad, fill_);
}

Spinner::Spinner(float size, Color color) : color_(color) { setPreferredSize(size, size); }

void Spinner::update(float dt) {
    t_ += dt;
    View::update(dt);
}

void Spinner::draw(Renderer& r) {
    const float size = std::min(width(), height());
    const float stroke = std::max(2.5f, size * 0.1f);
    const float radius = size * 0.5f - stroke * 0.5f;
    // Sweep breathes between ~20 and ~270 degrees while the head keeps rotating.
    const float cycle = std::fmod(t_, 1.4f) / 1.4f;
    const float grow = ease::inOutCubic(cycle < 0.5f ? cycle * 2 : 2 - cycle * 2);
    const float sweep = (0.12f + 0.63f * grow) * 2 * gfx::kPi;
    const float base = t_ * 2 * gfx::kPi * 0.8f + (cycle >= 0.5f ? (1 - grow) * 0.63f * 2 * gfx::kPi : 0.0f);
    const Vec2 c{width() * 0.5f, height() * 0.5f};
    r.drawArc(c, radius, stroke, base, sweep, color_);
}

}  // namespace karafin::ui
