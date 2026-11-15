#pragma once

#include <string>

#include "ui/widgets/label.hpp"

namespace karafin::screens {

// Overview clipped to a few lines. Only when the text doesn't fit does it take focus; A (or a
// tap) then opens the full text in a scrollable sheet.
class OverviewText : public ui::Label {
public:
    explicit OverviewText(int maxLines);

    void setSheetTitle(std::string title) { sheetTitle_ = std::move(title); }
    bool truncated() const { return truncated_; }

    void layout() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;
    void onFocus() override;
    void onBlur() override;

private:
    int maxLines_;
    bool truncated_ = false;
    std::string sheetTitle_;
    ui::Animated<float> focus_{0.0f};
};

// Centered card with a title and the full text, scrolled with the D-pad, ZL / ZR or touch.
void presentTextSheet(std::string title, std::string text);

}  // namespace karafin::screens
