#include "screens/widgets/avatar_tile.hpp"

#include <algorithm>

#include "gfx/text.hpp"
#include "screens/common.hpp"
#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kNameGap = 14;
}

AvatarTile::AvatarTile(std::string name, std::string imageUrl, gfx::Icon icon)
    : name_(std::move(name)), icon_(icon), color_(placeholderColor(name_).lighter(0.12f)) {
    setFocusable(true);
    setCullMargin(40);
    if (!imageUrl.empty()) image_ = gfx::ImageCache::instance().load(imageRequest(std::move(imageUrl), kSize, kSize));
    setPivot({0.5f, (kSize * 0.5f) / (kSize + 60)});
}

Vec2 AvatarTile::measure(Vec2 available) {
    (void)available;
    return {kSize + 24, kSize + kNameGap + 44};
}

void AvatarTile::onFocus() {
    focus_.springTo(1.0f, theme::motion::snappy);
    scale.springTo(1.08f, theme::motion::focus);
    setZ(5);
}

void AvatarTile::onBlur() {
    focus_.springTo(0.0f, theme::motion::standard);
    scale.springTo(1.0f, theme::motion::focus);
    setZ(0);
}

bool AvatarTile::activate() {
    scale.set(1.0f);
    scale.springTo(focused() ? 1.08f : 1.0f, theme::motion::bouncy);
    return View::activate();
}

bool AvatarTile::onInput(const InputEvent& e) {
    if (e.pressed() && e.button == Key::Y && focused() && onSecondary) {
        auto fn = onSecondary;
        fn();
        return true;
    }
    return View::onInput(e);
}

void AvatarTile::update(float dt) {
    focus_.update(dt);
    if (image_ && image_->ready() && reveal_.target() < 1.0f) reveal_.tweenTo(1.0f, theme::motion::imageFade);
    reveal_.update(dt);
    View::update(dt);
}

void AvatarTile::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const Rect circle{(width() - kSize) * 0.5f, 0, kSize, kSize};
    const Vec2 c = circle.center();
    const float rad = kSize * 0.5f;
    r.drawShadow(circle, rad, 18 + 20 * f, Color::black(0.4f + 0.3f * f), {0, 6 + 8 * f});
    if (f > 0.01f) r.drawShadow(circle, rad, 40, color::accentGlow.mulAlpha(0.7f * f), {0, 6}, 2);

    const gfx::Texture* tex = image_ ? image_->texture() : nullptr;
    const float reveal = tex ? std::clamp(reveal_.value(), 0.0f, 1.0f) : 0.0f;
    if (reveal < 1.0f) {
        r.fillCircle(c, rad, gfx::Color::lerp(color_, color_.lighter(0.15f), f));
        if (icon_ != gfx::Icon::None) {
            gfx::drawIcon(r, icon_, {c.x - 22, c.y - 22, 44, 44}, Color::white(0.9f), 1.2f);
        } else if (!name_.empty()) {
            const auto lead = static_cast<unsigned char>(name_[0]);
            const size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            const std::string initial = upperAscii(name_.substr(0, len));
            const gfx::TextStyle st{48, gfx::FontWeight::Display, 1.0f, 0.0f};
            auto l = gfx::text::layout(initial, st);
            const float capH = gfx::text::capHeight(st);
            r.drawText(*l, {c.x - l->width * 0.5f, c.y - capH * 0.5f - (l->firstBaseline - capH)}, Color::white(0.92f));
        }
    }
    if (tex && reveal > 0) r.drawImage(*tex, circle, {.fit = gfx::ImageFit::Cover, .radius = rad, .tint = Color::white(reveal)});
    if (f > 0.01f) r.strokeCircle(c, rad + 5, 3, color::focusRing.mulAlpha(f));

    if (marked_) {
        const Vec2 m{circle.right() - 12, circle.bottom() - 12};
        r.fillCircle(m, 15, color::background);
        r.fillCircle(m, 12, color::success);
        gfx::drawIcon(r, gfx::Icon::Check, {m.x - 7, m.y - 7, 14, 14}, Color::white(), 1.4f);
    }

    const Color ink = Color::lerp(color::textSecondary, color::textPrimary, f);
    auto name = gfx::text::layout(name_, type::headline, {.maxWidth = width(), .maxLines = 1});
    r.drawText(*name, {(width() - name->width) * 0.5f, circle.bottom() + kNameGap}, ink);
    if (!caption_.empty()) {
        auto cap = gfx::text::layout(caption_, type::footnote, {.maxWidth = width()});
        r.drawText(*cap, {(width() - cap->width) * 0.5f, circle.bottom() + kNameGap + name->height + 2},
                   color::textTertiary);
    }
}

}  // namespace karafin::screens
