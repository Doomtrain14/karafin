#pragma once

#include <string>

#include "ui/widgets/button.hpp"

namespace karafin::screens {

// ui::Button with a caption that fades in under it while focused (for icon-only buttons) and an
// optional "active" marker (favorite / watched toggles).
class ActionButton : public ui::Button {
public:
    ActionButton(std::string label, gfx::Icon icon, ui::ButtonStyle style = ui::ButtonStyle::Secondary,
                 std::string tooltip = {});

    void setTooltip(std::string tooltip) { tooltip_ = std::move(tooltip); }
    void setActive(bool active) { active_ = active; }
    bool active() const { return active_; }
    void setActiveColor(ui::Color c) { activeColor_ = c; }

    void onFocus() override;
    void onBlur() override;
    void update(float dt) override;
    void drawOverlay(ui::Renderer& r) override;

private:
    std::string tooltip_;
    bool active_ = false;
    ui::Color activeColor_ = ui::theme::color::accent;
    ui::Animated<float> tip_{0.0f};
    ui::Animated<float> mark_{0.0f};
};

}  // namespace karafin::screens
