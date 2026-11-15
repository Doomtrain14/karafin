#pragma once

#include <string>

#include "gfx/icons.hpp"
#include "ui/theme.hpp"
#include "ui/view.hpp"

namespace karafin::ui {

enum class ButtonStyle : uint8_t {
    Primary,    // accent fill; becomes white when focused
    Secondary,  // translucent fill; becomes white when focused
    Ghost,      // no fill at rest
    Icon,       // circular icon-only button
};

// Focusable button with spring focus scale, color crossfade and a press "dip".
//
//   auto* play = row->add<Button>("Play", gfx::Icon::Play, ButtonStyle::Primary);
//   play->onActivate = [this] { startPlayback(); };
class Button : public View {
public:
    explicit Button(std::string label = {}, gfx::Icon icon = gfx::Icon::None,
                    ButtonStyle style = ButtonStyle::Secondary);

    void setLabel(std::string label);
    const std::string& label() const { return label_; }
    void setIcon(gfx::Icon icon) { icon_ = icon; }
    void setStyle(ButtonStyle style) { style_ = style; }

    Vec2 measure(Vec2 available) override;
    void onFocus() override;
    void onBlur() override;
    bool activate() override;
    void onTouchDown() override;
    void onTouchCancel() override;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    std::string label_;
    gfx::Icon icon_;
    ButtonStyle style_;
    Animated<float> focus_{0.0f};
};

}  // namespace karafin::ui
