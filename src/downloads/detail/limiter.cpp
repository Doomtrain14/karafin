#include "downloads/detail/limiter.hpp"

#include <algorithm>

#include "core/thread.hpp"

namespace karafin::downloads::detail {

void BandwidthLimiter::setLimit(uint64_t bytesPerSecond) {
    std::lock_guard lock(mutex_);
    if (rate_ == bytesPerSecond) return;
    rate_ = bytesPerSecond;
    tokens_ = 0;
    last_ = Clock::now();
}

uint64_t BandwidthLimiter::limit() const {
    std::lock_guard lock(mutex_);
    return rate_;
}

double BandwidthLimiter::take(double& tokens, double rate, double elapsedSec, size_t bytes) {
    if (rate <= 0) return 0;
    const double burst = rate / 4.0;
    tokens = std::min(burst, tokens + rate * std::max(0.0, elapsedSec));
    tokens -= static_cast<double>(bytes);
    return tokens >= 0 ? 0.0 : -tokens / rate;
}

void BandwidthLimiter::acquire(size_t bytes, const CancelToken& cancel) {
    double wait = 0;
    {
        std::lock_guard lock(mutex_);
        if (rate_ == 0) return;
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        wait = take(tokens_, static_cast<double>(rate_), elapsed, bytes);
    }
    // The debt stays in the bucket, so sleeping it off here keeps the long-run rate at the
    // limit even when several jobs share it.
    while (wait > 0 && !cancel.cancelled()) {
        if (limit() == 0) return;  // throttling was switched off meanwhile
        const double slice = std::min(wait, 0.1);
        sleepMs(std::max(1, static_cast<int>(slice * 1000.0)));
        wait -= slice;
    }
}

}  // namespace karafin::downloads::detail
