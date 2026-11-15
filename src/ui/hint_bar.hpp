#pragma once

// Controller button glyphs and the bottom hint bar ("(A) Select  (B) Back").

#include <vector>

#include "gfx/renderer.hpp"
#include "ui/input.hpp"

namespace karafin::ui {

// Width of the glyph for `b` at `size` (height).
float buttonGlyphWidth(Key b, float size);
// Draws a Switch-style glyph centered at `center`. Face buttons are filled circles with the
// letter knocked out in `ink`; shoulder buttons are pills; d-pad draws a cross.
void drawButtonGlyph(gfx::Renderer& r, Key b, gfx::Vec2 center, float size, gfx::Color fill, gfx::Color ink);

class HintBar {
public:
    void setHints(const std::vector<Hint>& hints);
    void update(float dt);
    // Draws right-aligned along the bottom of a `width` x `height` viewport with a soft scrim.
    void render(gfx::Renderer& r, float width, float height, float visibility);

private:
    // Hints plus their measured labels, kept until the hints or the display scale change.
    struct Set {
        std::vector<Hint> hints;
        std::vector<gfx::TextLayoutPtr> labels;
        float scale = 0;
    };
    static void measure(Set& set);
    void drawSet(gfx::Renderer& r, Set& set, float width, float height, float alpha);

    Set current_;
    Set previous_;
    float fade_ = 1.0f;  // crossfade progress when hints change
};

}  // namespace karafin::ui
