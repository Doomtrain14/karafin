#include "ui/hint_bar.hpp"

#include <algorithm>
#include <cstddef>

#include "gfx/icons.hpp"
#include "ui/theme.hpp"

namespace karafin::ui {
namespace {

const char* glyphLabel(Key b) {
    switch (b) {
        case Key::A: return "A";
        case Key::B: return "B";
        case Key::X: return "X";
        case Key::Y: return "Y";
        case Key::L: return "L";
        case Key::R: return "R";
        case Key::ZL: return "ZL";
        case Key::ZR: return "ZR";
        case Key::LStick: return "L";
        case Key::RStick: return "R";
        default: return "";
    }
}

bool sameHints(const std::vector<Hint>& a, const std::vector<Hint>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].button != b[i].button || a[i].label != b[i].label) return false;
    return true;
}

}  // namespace

float buttonGlyphWidth(Key b, float size) {
    switch (b) {
        case Key::L:
        case Key::R: return size * 1.45f;
        case Key::ZL:
        case Key::ZR: return size * 1.75f;
        default: return size;
    }
}

void drawButtonGlyph(gfx::Renderer& r, Key b, gfx::Vec2 c, float size, gfx::Color fill, gfx::Color ink) {
    const float w = buttonGlyphWidth(b, size);
    const gfx::Rect box{c.x - w * 0.5f, c.y - size * 0.5f, w, size};
    gfx::TextStyle st{size * 0.58f, gfx::FontWeight::Bold, 1.0f, 0.0f};
    switch (b) {
        case Key::Plus:
        case Key::Minus: {
            r.fillCircle(c, size * 0.5f, fill);
            gfx::drawIcon(r, b == Key::Plus ? gfx::Icon::Plus : gfx::Icon::Minus,
                          {c.x - size * 0.32f, c.y - size * 0.32f, size * 0.64f, size * 0.64f}, ink, 1.5f);
            return;
        }
        case Key::Up:
        case Key::Down:
        case Key::Left:
        case Key::Right: {
            // D-pad cross with the pressed direction highlighted.
            const float arm = size * 0.34f, len = size * 0.5f;
            r.fillRoundRect({c.x - arm * 0.5f, c.y - len, arm, len * 2}, arm * 0.3f, fill.mulAlpha(0.45f));
            r.fillRoundRect({c.x - len, c.y - arm * 0.5f, len * 2, arm}, arm * 0.3f, fill.mulAlpha(0.45f));
            gfx::Rect hi;
            if (b == Key::Up) hi = {c.x - arm * 0.5f, c.y - len, arm, len};
            else if (b == Key::Down) hi = {c.x - arm * 0.5f, c.y, arm, len};
            else if (b == Key::Left) hi = {c.x - len, c.y - arm * 0.5f, len, arm};
            else hi = {c.x, c.y - arm * 0.5f, len, arm};
            r.fillRoundRect(hi, arm * 0.3f, fill);
            return;
        }
        case Key::L:
        case Key::R:
        case Key::ZL:
        case Key::ZR:
            r.fillRoundRect(box, size * 0.32f, fill);
            st.size = size * 0.52f;
            break;
        case Key::LStick:
        case Key::RStick:
            r.strokeCircle(c, size * 0.5f, size * 0.1f, fill);
            st.size = size * 0.5f;
            break;
        default:
            r.fillCircle(c, size * 0.5f, fill);
            break;
    }
    auto layout = gfx::text::layout(glyphLabel(b), st);
    const float capH = gfx::text::capHeight(st);
    // Optical centering on the cap height rather than the line box.
    const float y = c.y - capH * 0.5f - (layout->firstBaseline - capH);
    const bool ringed = b == Key::LStick || b == Key::RStick;
    r.drawText(*layout, {c.x - layout->width * 0.5f, y}, ringed ? fill : ink);
}

void HintBar::setHints(const std::vector<Hint>& hints) {
    if (sameHints(hints, hints_)) return;
    previous_ = hints_;
    hints_ = hints;
    fade_ = 0.0f;
}

void HintBar::update(float dt) { fade_ = std::min(1.0f, fade_ + dt / 0.18f); }

void HintBar::render(gfx::Renderer& r, float width, float height, float visibility) {
    if (visibility <= 0.01f) return;
    using namespace theme;
    const float barH = layout::hintBarHeight;
    r.pushAlpha(visibility);
    r.fillRect({0, height - barH - 36, width, barH + 36},
               gfx::Gradient::vertical(Color::black(0.0f), Color::black(0.55f)));

    auto drawSet = [&](const std::vector<Hint>& set, float alpha) {
        if (alpha <= 0.01f || set.empty()) return;
        const float glyph = 22;
        const float cy = height - barH * 0.5f - 2;
        // Too many hints for the bar: tighten the gaps, then drop the trailing (least basic)
        // ones so nothing runs past the left margin.
        const float avail = width - layout::marginX * 2;
        size_t count = set.size();
        float gap = 26;
        auto total = [&](size_t n, float g) {
            float w = 0;
            for (size_t i = 0; i < n; ++i)
                w += gfx::text::layout(set[i].label, type::caption)->width + 8 + buttonGlyphWidth(set[i].button, glyph);
            return w + g * static_cast<float>(n > 0 ? n - 1 : 0);
        };
        if (total(count, gap) > avail) gap = 14;
        while (count > 1 && total(count, gap) > avail) --count;
        float x = width - layout::marginX;
        r.pushAlpha(alpha);
        for (auto it = set.rbegin() + static_cast<std::ptrdiff_t>(set.size() - count); it != set.rend(); ++it) {
            auto label = gfx::text::layout(it->label, type::caption);
            x -= label->width;
            r.drawText(*label, {x, cy - label->height * 0.5f}, color::textSecondary);
            x -= 8;
            const float gw = buttonGlyphWidth(it->button, glyph);
            drawButtonGlyph(r, it->button, {x - gw * 0.5f, cy}, glyph, Color::white(0.9f), color::background);
            x -= gw + gap;
        }
        r.popAlpha();
    };
    const float e = fade_ * fade_ * (3 - 2 * fade_);
    drawSet(previous_, 1.0f - e);
    drawSet(hints_, e);
    r.popAlpha();
}

}  // namespace karafin::ui
