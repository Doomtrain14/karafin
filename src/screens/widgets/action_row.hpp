#pragma once

#include <functional>
#include <string>

#include "gfx/icons.hpp"
#include "gfx/image.hpp"
#include "ui/view.hpp"

namespace karafin::screens {

// Focusable list row: leading icon (or image), title, subtitle, trailing detail and chevron.
// Focus slides in a white plate and inverts the text (matches ui::ListMenu).
class ActionRow : public ui::View {
public:
    ActionRow(gfx::Icon icon, std::string title, std::string subtitle = {}, std::string detail = {});

    void setTitle(std::string t) { title_ = std::move(t); }
    void setSubtitle(std::string s);
    void setDetail(std::string d) { detail_ = std::move(d); }
    void setIcon(gfx::Icon icon) { icon_ = icon; }
    void setIconColor(ui::Color c) { iconColor_ = c; }
    void setImageUrl(const std::string& url);
    void setChevron(bool v) { chevron_ = v; }
    void setBusy(bool v) { busy_ = v; }

    // Y on the focused row (e.g. "Remove"); set a matching hint with setHints().
    std::function<void()> onSecondary;

    ui::Vec2 measure(ui::Vec2 available) override;
    void onFocus() override;
    void onBlur() override;
    bool activate() override;
    bool onInput(const ui::InputEvent& e) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    gfx::Icon icon_;
    std::string title_;
    std::string subtitle_;
    std::string detail_;
    gfx::ImagePtr image_;
    ui::Color iconColor_ = ui::theme::color::accentBright;
    bool chevron_ = true;
    bool busy_ = false;
    float spin_ = 0;
    ui::Animated<float> focus_{0.0f};
};

}  // namespace karafin::screens
