#include "ui/battery_indicator.hpp"

#include <algorithm>
#include <format>

#include "core/platform.hpp"
#include "core/settings.hpp"
#include "ui/root.hpp"
#include "ui/theme.hpp"

namespace karafin::ui {

void BatteryIndicator::poll() {
    const double t = now();
    if (t < nextPoll_ && (!label_ || label_->scale == gfx::text::displayScale())) return;
    nextPoll_ = t + 2.0;
    visible_ = Settings::instance().get("ui.show_battery", true);
    const auto state = visible_ ? platform::battery() : std::nullopt;
    if (!state) {
        percent_ = -1;
        label_.reset();
        return;
    }
    if (state->percent != percent_ || !label_ || label_->scale != gfx::text::displayScale())
        label_ = gfx::text::layout(std::format("{}%", state->percent), theme::type::caption);
    percent_ = state->percent;
    charging_ = state->charging;
}

float BatteryIndicator::draw(Renderer& r, float right, float centerY, gfx::Color color) {
    poll();
    if (!visible_ || percent_ < 0 || !label_) return 0;
    constexpr float bodyW = 24, bodyH = 12, capW = 2, gap = 6;
    const float bodyX = right - capW - bodyW;
    const gfx::Rect body{bodyX, centerY - bodyH * 0.5f, bodyW, bodyH};
    r.strokeRoundRect(body, 3, 1.5f, color.withAlpha(color.a * 0.85f));
    r.fillRoundRect({body.right() + 0.5f, centerY - 2.5f, capW, 5}, 1, color.withAlpha(color.a * 0.85f));

    gfx::Color fill = color;
    if (charging_) fill = theme::color::success;
    else if (percent_ <= 15) fill = theme::color::danger;
    const float level = std::clamp(static_cast<float>(percent_) / 100.0f, 0.0f, 1.0f);
    const gfx::Rect inner = body.inset(2.5f);
    if (level > 0) r.fillRoundRect({inner.x, inner.y, std::max(2.0f, inner.w * level), inner.h}, 1.5f, fill);

    const float textX = bodyX - gap - label_->width;
    r.drawText(*label_, {textX, centerY - label_->height * 0.5f}, color);
    return right - textX;
}

}  // namespace karafin::ui
