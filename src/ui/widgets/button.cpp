#include "ui/widgets/button.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/text.hpp"

namespace karafin::ui {

Button::Button(std::string label, gfx::Icon icon, ButtonStyle style)
    : label_(std::move(label)), icon_(icon), style_(style) {
    setFocusable(true);
    setCullMargin(30);
}

void Button::setLabel(std::string label) {
    if (label == label_) return;
    label_ = std::move(label);
    setNeedsLayout();
}

Vec2 Button::measure(Vec2 available) {
    (void)available;
    const Vec2 pref = preferredSize();
    if (pref.x >= 0 && pref.y >= 0) return pref;
    Vec2 out;
    if (style_ == ButtonStyle::Icon) {
        out = {theme::button::iconButtonSize, theme::button::iconButtonSize};
    } else {
        float w = theme::button::paddingX * 2;
        if (!label_.empty()) w += gfx::text::measure(label_, theme::type::button).x;
        if (icon_ != gfx::Icon::None) w += theme::button::iconSize + (label_.empty() ? 0 : 10);
        out = {std::ceil(w), theme::button::height};
    }
    if (pref.x >= 0) out.x = pref.x;
    if (pref.y >= 0) out.y = pref.y;
    return out;
}

void Button::onFocus() {
    if (focusGainSilent()) {
        focus_.set(1.0f);
        scale.set(theme::button::focusScale);
        setZ(1);
        return;
    }
    focus_.springTo(1.0f, theme::motion::snappy);
    scale.springTo(theme::button::focusScale, theme::motion::focus);
    setZ(1);
}

void Button::onBlur() {
    focus_.springTo(0.0f, theme::motion::snappy);
    scale.springTo(1.0f, theme::motion::focus);
    setZ(0);
}

bool Button::activate() {
    if (!enabled()) return false;
    // Press dip: drop quickly, then spring back to the focused size.
    scale.set(focused() ? theme::button::focusScale * 0.94f : 0.94f);
    scale.springTo(focused() ? theme::button::focusScale : 1.0f, theme::motion::bouncy);
    return View::activate();
}

void Button::onTouchDown() { scale.springTo(0.95f, theme::motion::snappy); }

void Button::onTouchCancel() { scale.springTo(focused() ? theme::button::focusScale : 1.0f, theme::motion::focus); }

void Button::update(float dt) {
    focus_.update(dt);
    View::update(dt);
}

void Button::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const Rect b = bounds();
    const bool iconOnly = style_ == ButtonStyle::Icon;
    const float rad = iconOnly ? b.h * 0.5f : radius::md;

    // Shadow grows with focus.
    if (f > 0.01f) r.drawShadow(b, rad, 12 + 16 * f, Color::black(0.45f * f), {0, 6 + 6 * f});

    Color rest;
    switch (style_) {
        case ButtonStyle::Primary: rest = color::accent; break;
        case ButtonStyle::Ghost: rest = Color::white(0.0f); break;
        case ButtonStyle::Secondary:
        case ButtonStyle::Icon: rest = color::buttonSecondary; break;
    }
    const Color fill = Color::lerp(rest, color::buttonSecondaryFocused, f);
    if (style_ == ButtonStyle::Primary && f < 0.99f) {
        // Accent gradient at rest fading into the focused white.
        r.fillRoundRect(b, rad, accentGradient);
        if (f > 0.01f) r.fillRoundRect(b, rad, color::buttonSecondaryFocused.mulAlpha(f));
    } else if (fill.a > 0.003f) {
        r.fillRoundRect(b, rad, fill);
    }
    if (!enabled()) r.fillRoundRect(b, rad, Color::black(0.45f));

    const Color restInk = style_ == ButtonStyle::Primary ? color::textOnAccent : color::textPrimary;
    const Color ink = Color::lerp(restInk, color::textOnLight, f).mulAlpha(enabled() ? 1.0f : 0.5f);

    if (iconOnly) {
        const float s = button::iconSize * 1.1f;
        gfx::drawIcon(r, icon_, {(b.w - s) * 0.5f, (b.h - s) * 0.5f, s, s}, ink);
        return;
    }
    const float iconW = icon_ != gfx::Icon::None ? button::iconSize : 0.0f;
    const float gap = (iconW > 0 && !label_.empty()) ? 10.0f : 0.0f;
    // Narrower than its natural size (fixed preferred width): keep half the padding and ellipsize.
    const float maxLabelW = std::max(10.0f, b.w - button::paddingX - iconW - gap);
    auto layout = gfx::text::layout(label_, type::button, {.maxWidth = maxLabelW});
    const float contentW = iconW + gap + layout->width;
    float x = (b.w - contentW) * 0.5f;
    if (iconW > 0) {
        gfx::drawIcon(r, icon_, {x, (b.h - iconW) * 0.5f, iconW, iconW}, ink);
        x += iconW + gap;
    }
    const float capH = gfx::text::capHeight(type::button);
    const float y = (b.h - capH) * 0.5f - (layout->firstBaseline - capH);
    r.drawText(*layout, {x, y}, ink);
}

}  // namespace karafin::ui
