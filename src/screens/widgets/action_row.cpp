#include "screens/widgets/action_row.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/text.hpp"
#include "screens/common.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kHeight = 60;
constexpr float kHeightTall = 72;
constexpr float kPadX = 18;
constexpr float kLead = 40;
}  // namespace

ActionRow::ActionRow(gfx::Icon icon, std::string title, std::string subtitle, std::string detail)
    : icon_(icon), title_(std::move(title)), subtitle_(std::move(subtitle)), detail_(std::move(detail)) {
    setFocusable(true);
    setCullMargin(12);
}

void ActionRow::setSubtitle(std::string s) {
    const bool changed = s.empty() != subtitle_.empty();
    subtitle_ = std::move(s);
    if (changed && parent()) parent()->setNeedsLayout();
}

void ActionRow::setImageUrl(const std::string& url) {
    image_ = url.empty() ? nullptr : gfx::ImageCache::instance().load(imageRequest(url, kLead, kLead));
}

Vec2 ActionRow::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    return {pref.x >= 0 ? pref.x : available.x, pref.y >= 0 ? pref.y : (subtitle_.empty() ? kHeight : kHeightTall)};
}

void ActionRow::onFocus() {
    focus_.springTo(1.0f, theme::motion::snappy);
    scale.springTo(1.015f, theme::motion::focus);
    setZ(1);
}

void ActionRow::onBlur() {
    focus_.springTo(0.0f, theme::motion::snappy);
    scale.springTo(1.0f, theme::motion::focus);
    setZ(0);
}

bool ActionRow::activate() {
    if (!enabled()) return false;
    scale.set(0.98f);
    scale.springTo(focused() ? 1.015f : 1.0f, theme::motion::bouncy);
    return View::activate();
}

bool ActionRow::onInput(const InputEvent& e) {
    if (e.pressed() && e.button == Key::Y && focused() && onSecondary) {
        auto fn = onSecondary;
        fn();
        return true;
    }
    return View::onInput(e);
}

void ActionRow::update(float dt) {
    focus_.update(dt);
    if (busy_) spin_ = std::fmod(spin_ + dt * 1.3f, 1.0f);
    View::update(dt);
}

void ActionRow::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const Rect b = bounds();
    if (f > 0.01f) {
        r.drawShadow(b, radius::md, 18, Color::black(0.35f * f), {0, 6});
        r.fillRoundRect(b, radius::md, Color::white(0.95f * f));
    } else {
        r.fillRoundRect(b, radius::md, Color::white(0.04f));
    }
    const Color ink = Color::lerp(color::textPrimary, color::textOnLight, f);
    const Color sub = Color::lerp(color::textSecondary, Color(0.04f, 0.04f, 0.07f, 0.62f), f);
    float x = kPadX;
    const float h = b.h;
    if (image_ || icon_ != gfx::Icon::None) {
        const Rect lead{x, (h - kLead) * 0.5f, kLead, kLead};
        const gfx::Texture* tex = image_ ? image_->texture() : nullptr;
        if (tex) {
            r.drawImage(*tex, lead, {.fit = gfx::ImageFit::Cover, .radius = kLead * 0.5f});
        } else {
            r.fillCircle(lead.center(), kLead * 0.5f, Color::lerp(iconColor_.withAlpha(0.16f), iconColor_.withAlpha(0.22f), f));
            gfx::drawIcon(r, icon_, lead.inset(10), Color::lerp(iconColor_, color::accentDeep, f));
        }
        x += kLead + 14;
    }
    float right = b.w - kPadX;
    if (busy_) {
        const Vec2 c{right - 10, h * 0.5f};
        r.drawArc(c, 9, 2.5f, spin_ * 2 * gfx::kPi, gfx::kPi * 1.3f, ink);
        right -= 30;
    } else if (chevron_) {
        gfx::drawIcon(r, gfx::Icon::ChevronRight, {right - 18, (h - 18) * 0.5f, 18, 18}, sub);
        right -= 30;
    }
    if (!detail_.empty()) {
        auto d = gfx::text::layout(detail_, type::subhead, {.maxWidth = std::max(60.0f, b.w * 0.4f)});
        right -= d->width;
        r.drawText(*d, {right, (h - d->height) * 0.5f}, sub);
        right -= 14;
    }
    const float maxW = std::max(10.0f, right - x);
    auto title = gfx::text::layout(title_, type::headline, {.maxWidth = maxW});
    if (subtitle_.empty()) {
        r.drawText(*title, {x, (h - title->height) * 0.5f}, ink);
    } else {
        auto s = gfx::text::layout(subtitle_, type::caption, {.maxWidth = maxW});
        const float total = title->height + 2 + s->height;
        const float y = (h - total) * 0.5f;
        r.drawText(*title, {x, y}, ink);
        r.drawText(*s, {x, y + title->height + 2}, sub);
    }
}

}  // namespace karafin::screens
