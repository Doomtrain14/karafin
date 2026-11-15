#include "ui/widgets/label.hpp"

#include <cmath>

namespace karafin::ui {

Label::Label(std::string text, gfx::TextStyle style, Color c) : color(c), text_(std::move(text)), style_(style) {}

void Label::setText(std::string text) {
    if (text == text_) return;
    text_ = std::move(text);
    setNeedsLayout();
}

void Label::setStyle(const gfx::TextStyle& style) {
    style_ = style;
    setNeedsLayout();
}

void Label::setMaxLines(int lines) {
    maxLines_ = lines;
    setNeedsLayout();
}

void Label::setAlign(gfx::TextAlign align) { align_ = align; }

gfx::TextOptions Label::options(float maxWidth) const {
    gfx::TextOptions o;
    o.maxWidth = std::isfinite(maxWidth) && maxWidth > 0 ? maxWidth : 0.0f;
    o.maxLines = maxLines_;
    o.align = align_;
    return o;
}

Vec2 Label::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    const float maxW = pref.x >= 0 ? pref.x : available.x;
    auto layout = gfx::text::layout(text_, style_, options(maxW));
    Vec2 out{std::ceil(layout->width), layout->height};
    if (pref.x >= 0) out.x = pref.x;
    if (pref.y >= 0) out.y = pref.y;
    return out;
}

void Label::update(float dt) {
    color.update(dt);
    View::update(dt);
}

void Label::draw(Renderer& r) {
    if (text_.empty()) return;
    auto layout = gfx::text::layout(text_, style_, options(width()));
    float x = 0;
    if (align_ == gfx::TextAlign::Center) x = (width() - layout->width) * 0.5f;
    else if (align_ == gfx::TextAlign::Right) x = width() - layout->width;
    const float y = vcenter_ ? (height() - layout->height) * 0.5f : 0.0f;
    r.drawText(*layout, {x, y}, color.value());
}

IconView::IconView(gfx::Icon icon, Color c, float size) : color(c), icon_(icon) { setPreferredSize(size, size); }

void IconView::update(float dt) {
    color.update(dt);
    View::update(dt);
}

void IconView::draw(Renderer& r) { gfx::drawIcon(r, icon_, bounds(), color.value()); }

}  // namespace karafin::ui
