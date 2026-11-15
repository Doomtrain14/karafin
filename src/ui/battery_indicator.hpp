#pragma once

#include <string>

#include "gfx/renderer.hpp"
#include "gfx/text.hpp"

namespace karafin::ui {

using gfx::Renderer;

// "87%" with a small battery glyph (and a bolt while charging), drawn right-aligned. Polls psm and the `ui.show_battery`
// setting every few seconds rather than every frame.
class BatteryIndicator {
public:
    // Draws so the indicator ends at `right`, vertically centred on `centerY`. Returns the width
    // used (0 when hidden or the battery level is unavailable).
    float draw(Renderer& r, float right, float centerY, gfx::Color color);

private:
    void poll();

    double nextPoll_ = 0;
    bool visible_ = false;
    int percent_ = -1;
    bool charging_ = false;
    gfx::TextLayoutPtr label_;
};

}  // namespace karafin::ui
