#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include "core/cancel.hpp"

// Global token-bucket bandwidth limiter shared by all download jobs (one budget for all of
// them). Disabled (unlimited) unless enabled; DownloadManager::setThrottled() toggles it while
// the player streams.
namespace karafin::downloads::detail {

class BandwidthLimiter {
public:
    using Clock = std::chrono::steady_clock;

    // bytesPerSecond == 0 -> unlimited.
    void setLimit(uint64_t bytesPerSecond);
    uint64_t limit() const;

    // Blocks the calling (download) thread until `bytes` fit the budget. Sleeps in short slices
    // so a cancel or a limit change takes effect within ~100 ms.
    void acquire(size_t bytes, const CancelToken& cancel);

    // Pure bucket step (exposed for the self-test): refills `tokens` for `elapsedSec` at `rate`
    // (capped at a quarter second of burst), takes `bytes`, returns the seconds to wait (0 if
    // the budget covers it). `tokens` may go negative (debt).
    static double take(double& tokens, double rate, double elapsedSec, size_t bytes);

private:
    mutable std::mutex mutex_;
    uint64_t rate_ = 0;
    double tokens_ = 0;
    Clock::time_point last_ = Clock::now();
};

}  // namespace karafin::downloads::detail
