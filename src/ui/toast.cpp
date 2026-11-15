#include "ui/toast.hpp"

#include <algorithm>

#include "ui/theme.hpp"

namespace karafin::ui {
namespace {
constexpr size_t kMaxToasts = 3;
constexpr float kToastHeight = 46;
constexpr float kToastGap = 10;

Color kindColor(ToastKind k) {
    switch (k) {
        case ToastKind::Success: return theme::color::success;
        case ToastKind::Warning: return theme::color::warning;
        case ToastKind::Error: return theme::color::danger;
        case ToastKind::Info: break;
    }
    return theme::color::accent;
}

gfx::Icon kindIcon(ToastKind k) {
    switch (k) {
        case ToastKind::Success: return gfx::Icon::Check;
        case ToastKind::Warning: return gfx::Icon::Warning;
        case ToastKind::Error: return gfx::Icon::Close;
        case ToastKind::Info: break;
    }
    return gfx::Icon::Info;
}
}  // namespace

void ToastHost::show(std::string text, ToastKind kind, gfx::Icon icon, float duration) {
    // Too many: retire the oldest visible one early.
    size_t live = 0;
    for (auto& t : toasts_)
        if (!t.leaving) ++live;
    if (live >= kMaxToasts) {
        for (auto& t : toasts_) {
            if (!t.leaving) {
                t.leaving = true;
                t.appear.springTo(0.0f, {0.25f, 1.0f});
                break;
            }
        }
    }
    Toast t;
    t.text = std::move(text);
    t.kind = kind;
    t.icon = icon == gfx::Icon::None ? kindIcon(kind) : icon;
    t.remaining = duration;
    t.appear.springTo(1.0f, theme::motion::bouncy);
    toasts_.push_back(std::move(t));
}

void ToastHost::update(float dt) {
    for (auto& t : toasts_) {
        t.appear.update(dt);
        t.slot.update(dt);
        if (!t.leaving) {
            t.remaining -= dt;
            if (t.remaining <= 0) {
                t.leaving = true;
                t.appear.springTo(0.0f, {0.3f, 1.0f});
            }
        }
    }
    while (!toasts_.empty() && toasts_.front().leaving && toasts_.front().appear.value() <= 0.01f &&
           !toasts_.front().appear.animating())
        toasts_.pop_front();
    // Newest at the bottom; older ones move up.
    float slot = 0;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        if (it->slot.target() != slot) it->slot.springTo(slot, theme::motion::standard);
        if (!it->leaving) slot += 1.0f;
    }
}

void ToastHost::render(gfx::Renderer& r, float screenWidth, float bottom) {
    using namespace theme;
    for (auto& t : toasts_) {
        const float a = std::clamp(t.appear.value(), 0.0f, 1.2f);
        if (a <= 0.01f) continue;
        auto layout = gfx::text::layout(t.text, type::callout, {.maxWidth = screenWidth - theme::layout::marginX * 2 - 76});
        const float iconSize = 20;
        const float w = layout->width + iconSize + 12 + 22 * 2;
        const float x = (screenWidth - w) * 0.5f;
        const float y = bottom - kToastHeight - t.slot.value() * (kToastHeight + kToastGap) + (1 - a) * 28;
        const gfx::Rect box{x, y, w, kToastHeight};
        const float alpha = std::min(1.0f, a);

        r.pushAlpha(alpha);
        r.pushScale(0.92f + 0.08f * std::min(1.0f, a), box.center());
        r.drawShadow(box, kToastHeight * 0.5f, 26, Color::black(0.5f), {0, 10});
        r.fillRoundRect(box, kToastHeight * 0.5f, color::glassFill);
        r.strokeRoundRect(box, kToastHeight * 0.5f, 1, color::stroke);
        const Color kc = kindColor(t.kind);
        const Vec2 ic{x + 22 + iconSize * 0.5f, y + kToastHeight * 0.5f};
        r.fillCircle(ic, 14, kc.withAlpha(0.22f));
        gfx::drawIcon(r, t.icon, {ic.x - 8, ic.y - 8, 16, 16}, kc, 1.2f);
        r.drawText(*layout, {x + 22 + iconSize + 12, y + (kToastHeight - layout->height) * 0.5f}, color::textPrimary);
        r.popTransform();
        r.popAlpha();
    }
}

}  // namespace karafin::ui
