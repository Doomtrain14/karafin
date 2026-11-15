#pragma once

#include <string>

#include "gfx/icons.hpp"
#include "gfx/text.hpp"
#include "ui/theme.hpp"
#include "ui/view.hpp"

namespace karafin::ui {

// Text view. Single line with ellipsis by default; setMaxLines(0 or n) wraps to the frame width.
class Label : public View {
public:
    explicit Label(std::string text = {}, gfx::TextStyle style = theme::type::body,
                   Color color = theme::color::textPrimary);

    void setText(std::string text);
    const std::string& text() const { return text_; }
    void setStyle(const gfx::TextStyle& style);
    const gfx::TextStyle& style() const { return style_; }
    // Animated text color (springTo for focus transitions).
    Animated<Color> color;
    void setColor(Color c) { color.set(c); }
    void setMaxLines(int lines);
    void setAlign(gfx::TextAlign align);
    void setVerticalCenter(bool v) { vcenter_ = v; }

    Vec2 measure(Vec2 available) override;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    gfx::TextOptions options(float maxWidth) const;

    std::string text_;
    gfx::TextStyle style_;
    int maxLines_ = 1;
    gfx::TextAlign align_ = gfx::TextAlign::Left;
    bool vcenter_ = false;
};

// A vector icon filling its frame.
class IconView : public View {
public:
    explicit IconView(gfx::Icon icon = gfx::Icon::None, Color color = theme::color::textPrimary, float size = 24);
    void setIcon(gfx::Icon icon) { icon_ = icon; }
    Animated<Color> color;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    gfx::Icon icon_;
};

}  // namespace karafin::ui
