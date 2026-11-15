#pragma once

#include "ui/theme.hpp"
#include "ui/view.hpp"

namespace karafin::ui {

// Rounded progress bar. setValue() springs to the new value; indeterminate shows a gliding
// segment.
class ProgressBar : public View {
public:
    ProgressBar();
    void setValue(float v, bool animated = true);
    float value() const { return value_.target(); }
    void setIndeterminate(bool v) { indeterminate_ = v; }
    void setColors(Color track, Color fill) {
        track_ = track;
        fill_ = fill;
        gradient_ = false;
    }
    // Secondary (buffered) amount drawn between track and fill.
    void setBuffer(float v) { buffer_ = v; }

    Vec2 measure(Vec2 available) override;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    Animated<float> value_{0.0f};
    float buffer_ = 0;
    float phase_ = 0;
    Color track_ = theme::color::progressTrack;
    Color fill_ = theme::color::accent;
    bool gradient_ = true;
    bool indeterminate_ = false;
};

// Material-style spinner: a rotating arc that grows and shrinks.
class Spinner : public View {
public:
    explicit Spinner(float size = 36, Color color = theme::color::textPrimary);
    void setColor(Color c) { color_ = c; }
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    Color color_;
    float t_ = 0;
};

}  // namespace karafin::ui
