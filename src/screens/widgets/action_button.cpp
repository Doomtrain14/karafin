#include "screens/widgets/action_button.hpp"

#include <algorithm>

#include "gfx/text.hpp"

namespace karafin::screens {

using namespace karafin::ui;

ActionButton::ActionButton(std::string label, gfx::Icon icon, ButtonStyle style, std::string tooltip)
    : Button(std::move(label), icon, style), tooltip_(std::move(tooltip)) {
    setCullMargin(60);
}

void ActionButton::onFocus() {
    Button::onFocus();
    if (focusGainSilent()) tip_.set(1.0f);
    else tip_.springTo(1.0f, theme::motion::snappy);
}

void ActionButton::onBlur() {
    Button::onBlur();
    tip_.springTo(0.0f, theme::motion::snappy);
}

void ActionButton::update(float dt) {
    mark_.springTo(active_ ? 1.0f : 0.0f, theme::motion::bouncy);
    tip_.update(dt);
    mark_.update(dt);
    Button::update(dt);
}

void ActionButton::drawOverlay(Renderer& r) {
    const Rect b = bounds();
    const float m = std::clamp(mark_.value(), 0.0f, 1.2f);
    if (m > 0.01f) {
        // Accent badge with a check in the top-right corner.
        const Vec2 c{b.right() - 5, b.y + 5};
        const float rad = 8.0f * m;
        r.fillCircle(c, rad + 2, theme::color::background);
        r.fillCircle(c, rad, activeColor_);
        if (m > 0.5f) gfx::drawIcon(r, gfx::Icon::Check, {c.x - 5.5f, c.y - 5.5f, 11, 11}, Color::white(), 1.4f);
    }
    const float t = std::clamp(tip_.value(), 0.0f, 1.0f);
    if (t > 0.01f && !tooltip_.empty()) {
        auto layout = gfx::text::layout(tooltip_, theme::type::caption, {.maxWidth = 360});
        // Centered under the button, but kept on screen (long names on buttons at the edges).
        const float edge = theme::layout::marginX * 0.5f;
        const float originX = rootPosition().x;
        const float x = std::clamp((b.w - layout->width) * 0.5f, edge - originX,
                                   std::max(edge - originX, theme::layout::screenWidth - edge - originX - layout->width));
        const float y = b.bottom() + 10 + (1.0f - t) * -6.0f;
        r.drawText(*layout, {x, y}, theme::color::textPrimary.mulAlpha(t));
    }
}

}  // namespace karafin::screens
