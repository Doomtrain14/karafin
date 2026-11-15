#pragma once

#include <algorithm>
#include <cmath>

namespace karafin::player {

class PlaybackSwipe {
public:
    enum class Control { None, Brightness, Volume };

    // Without `brightness` (docked: a TV's backlight can't be changed) both sides control volume.
    bool begin(float x, float y, float width, float height, bool blocked, bool brightness = true) {
        cancel();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
            width <= 0 || height <= 0)
            return false;
        startX_ = x;
        startY_ = y;
        height_ = height;
        left_ = x < width * 0.5f;
        candidate_ = blocked ? Control::None : left_ && brightness ? Control::Brightness : Control::Volume;
        control_ = Control::None;
        return true;
    }

    Control move(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            cancel();
            return Control::None;
        }
        if (candidate_ == Control::None || control_ != Control::None) return control_;
        const float dx = std::fabs(x - startX_), dy = std::fabs(y - startY_);
        if (std::max(dx, dy) < 16) return Control::None;
        if (dy > dx * 1.25f) control_ = candidate_;
        else candidate_ = Control::None;
        return control_;
    }

    float value(float y, float initial, float minimum) const {
        return std::clamp(initial + (startY_ - y) / (height_ * 0.6f), minimum, 1.0f);
    }

    // The volume swipe spans 0..kMaxVolume percent (mpv `volume-max`); above 100% mpv amplifies.
    static constexpr int kMaxVolume = 200;
    static float volumeFraction(double percent) {
        if (!std::isfinite(percent)) return 0;
        return static_cast<float>(std::clamp(percent / kMaxVolume, 0.0, 1.0));
    }
    static int volumePercent(float fraction) {
        return static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * kMaxVolume));
    }

    void cancel() { candidate_ = control_ = Control::None; }
    Control control() const { return control_; }
    bool left() const { return left_; }

private:
    float startX_ = 0, startY_ = 0, height_ = 720;
    Control candidate_ = Control::None, control_ = Control::None;
    bool left_ = false;
};

}  // namespace karafin::player
