#include "screens/widgets/chips.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/text.hpp"
#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kHeight = 24;
constexpr float kPadX = 8;
constexpr float kGap = 10;
constexpr float kIcon = 14;
const gfx::TextStyle& textStyle(ChipStyle s) {
    static const gfx::TextStyle plain = theme::type::callout;
    static const gfx::TextStyle chip{12, gfx::FontWeight::SemiBold, 1.0f, 0.4f};
    return s == ChipStyle::Text ? plain : chip;
}
}  // namespace

void ChipRow::setChips(std::vector<Chip> chips) {
    chips_ = std::move(chips);
    if (parent()) parent()->setNeedsLayout();
}

float ChipRow::chipWidth(const Chip& c) const {
    const float tw = gfx::text::measure(c.text, textStyle(c.style)).x;
    const float iw = c.icon != gfx::Icon::None ? kIcon + (c.text.empty() ? 0.0f : 4.0f) : 0.0f;
    return tw + iw + (c.style == ChipStyle::Text ? 0.0f : kPadX * 2);
}

Vec2 ChipRow::measure(Vec2 available) {
    float w = 0;
    for (size_t i = 0; i < chips_.size(); ++i) w += chipWidth(chips_[i]) + (i > 0 ? kGap : 0.0f);
    const Vec2 pref = preferredSize();
    if (std::isfinite(available.x) && available.x > 0) w = std::min(w, available.x);
    return {pref.x >= 0 ? pref.x : std::ceil(w), pref.y >= 0 ? pref.y : (chips_.empty() ? 0.0f : kHeight)};
}

void ChipRow::draw(Renderer& r) {
    float x = 0;
    for (const auto& c : chips_) {
        float w = chipWidth(c);
        // Plain text (genres, episode name) that doesn't fit is ellipsized; pills are dropped.
        float textMax = 0;
        if (x + w > width() + 1) {
            const float room = width() - x;
            if (c.style != ChipStyle::Text || c.icon != gfx::Icon::None || room < 48) break;
            w = textMax = room;
        }
        const Rect box{x, 0, w, kHeight};
        Color ink = theme::color::textPrimary;
        switch (c.style) {
            case ChipStyle::Text: ink = theme::color::textSecondary; break;
            case ChipStyle::Outline: r.strokeRoundRect(box, 5, 1.2f, Color::white(0.45f)); break;
            case ChipStyle::Filled: r.fillRoundRect(box, 5, Color::white(0.14f)); break;
            case ChipStyle::Accent:
                r.fillRoundRect(box, 5, theme::accentGradient);
                ink = theme::color::textOnAccent;
                break;
        }
        const auto& st = textStyle(c.style);
        float tx = x + (c.style == ChipStyle::Text ? 0.0f : kPadX);
        if (c.icon != gfx::Icon::None) {
            const Color ic = c.icon == gfx::Icon::Star ? theme::color::warning : ink;
            gfx::drawIcon(r, c.icon, {tx, (kHeight - kIcon) * 0.5f, kIcon, kIcon}, ic, 1.2f);
            tx += kIcon + (c.text.empty() ? 0.0f : 4.0f);
        }
        if (!c.text.empty()) {
            auto layout = gfx::text::layout(c.text, st, {.maxWidth = textMax});
            const float capH = gfx::text::capHeight(st);
            const float y = (kHeight - capH) * 0.5f - (layout->firstBaseline - capH);
            r.drawText(*layout, {tx, y}, ink);
        }
        x += w + kGap;
    }
}

}  // namespace karafin::screens
