#include "screens/widgets/storage_bar.hpp"

#include <algorithm>

#include "gfx/text.hpp"
#include "screens/common.hpp"
#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kBarH = 10;
constexpr float kHeight = 40;
}  // namespace

void StorageBar::setUsage(uint64_t downloadBytes, uint64_t freeBytes, uint64_t totalBytes) {
    downloads_ = downloadBytes;
    free_ = freeBytes;
    total_ = totalBytes;
    if (total_ > 0) {
        const double used = static_cast<double>(total_ - std::min(free_, total_));
        dlFrac_.springTo(static_cast<float>(std::min<double>(static_cast<double>(downloads_), used) / static_cast<double>(total_)),
                         theme::motion::gentle);
        usedFrac_.springTo(static_cast<float>(used / static_cast<double>(total_)), theme::motion::gentle);
        const uint64_t usedBytes = total_ - std::min(free_, total_);
        const uint64_t other = usedBytes - std::min(downloads_, usedBytes);
        legend_[0] = "Downloads " + formatBytes(downloads_);
        legend_[1] = "Other " + formatBytes(other);
        legend_[2] = "Free " + formatBytes(free_);
        totalText_ = "of " + formatBytes(total_);
    }
}

Vec2 StorageBar::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    return {pref.x >= 0 ? pref.x : available.x, pref.y >= 0 ? pref.y : kHeight};
}

void StorageBar::update(float dt) {
    dlFrac_.update(dt);
    usedFrac_.update(dt);
    View::update(dt);
}

void StorageBar::draw(Renderer& r) {
    using namespace theme;
    const float w = width();
    const Rect track{0, 0, w, kBarH};
    r.fillRoundRect(track, kBarH * 0.5f, color::progressTrack.mulAlpha(0.6f));
    if (total_ == 0) {
        r.drawText("Storage information unavailable", {0, kBarH + 8}, type::caption, color::textTertiary);
        return;
    }
    const float used = std::clamp(usedFrac_.value(), 0.0f, 1.0f);
    const float dl = std::clamp(dlFrac_.value(), 0.0f, used);
    if (used > 0) r.fillRoundRect({0, 0, std::max(kBarH, w * used), kBarH}, kBarH * 0.5f, Color::white(0.32f));
    if (dl > 0) r.fillRoundRect({0, 0, std::max(kBarH, w * dl), kBarH}, kBarH * 0.5f, accentGradient);

    // Legend: swatch + text pairs.
    float x = 0;
    const float y = kBarH + 12;
    auto legend = [&](Color swatch, const std::string& text) {
        r.fillCircle({x + 5, y + 8}, 5, swatch);
        x += 16;
        const Vec2 s = r.drawText(text, {x, y}, type::caption, color::textSecondary);
        x += s.x + 22;
    };
    legend(color::accent, legend_[0]);
    legend(Color::white(0.32f), legend_[1]);
    legend(color::progressTrack, legend_[2]);
    auto totalText = gfx::text::layout(totalText_, type::caption);
    if (x + totalText->width < w) r.drawText(*totalText, {w - totalText->width, y}, color::textTertiary);
}

}  // namespace karafin::screens
