#pragma once

#include <algorithm>

namespace karafin::player {

class UpNextTimer {
public:
    void start(bool enabled) { remaining_ = enabled ? kSeconds : 0; }
    void cancel() { remaining_ = 0; }
    double remaining() const { return remaining_; }
    double progress() const { return 1.0 - remaining_ / kSeconds; }

    // Fires once, and only while playback is allowed to advance.
    bool tick(double dt, bool advancing) {
        if (!advancing || remaining_ <= 0) return false;
        remaining_ = std::max(0.0, remaining_ - std::max(0.0, dt));
        return remaining_ == 0;
    }

private:
    static constexpr double kSeconds = 10;
    double remaining_ = 0;
};

}  // namespace karafin::player
