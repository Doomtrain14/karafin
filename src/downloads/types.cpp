#include "downloads/types.hpp"

#include <algorithm>
#include <cmath>

namespace karafin::downloads {

std::string_view stateName(State s) {
    switch (s) {
        case State::Queued: return "Queued";
        case State::Probing: return "Probing";
        case State::Downloading: return "Downloading";
        case State::Paused: return "Paused";
        case State::WaitingForNetwork: return "WaitingForNetwork";
        case State::Completed: return "Completed";
        case State::Failed: return "Failed";
    }
    return "Queued";
}

std::optional<State> parseState(std::string_view name) {
    static constexpr State kAll[] = {State::Queued,  State::Probing,           State::Downloading, State::Paused,
                                     State::WaitingForNetwork, State::Completed, State::Failed};
    for (State s : kAll)
        if (stateName(s) == name) return s;
    return std::nullopt;
}

bool canTransition(State from, State to) {
    if (from == to) return true;
    switch (from) {
        case State::Queued: return to == State::Probing || to == State::Paused || to == State::Failed;
        case State::Probing:
            return to == State::Downloading || to == State::WaitingForNetwork || to == State::Paused ||
                   to == State::Failed || to == State::Completed;
        case State::Downloading:
            return to == State::Completed || to == State::Failed || to == State::Paused ||
                   to == State::WaitingForNetwork || to == State::Probing;
        case State::WaitingForNetwork: return to == State::Probing || to == State::Paused || to == State::Failed;
        // A job that already finished renaming the file may complete after a late pause().
        case State::Paused: return to == State::Queued || to == State::Completed;
        case State::Failed: return to == State::Queued;
        // Restore found the media file missing.
        case State::Completed: return to == State::Failed;
    }
    return false;
}

bool isActive(State s) { return s == State::Probing || s == State::Downloading || s == State::WaitingForNetwork; }

void ThroughputMeter::update(const std::vector<DownloadInfo>& downloads, double dtSeconds) {
    constexpr double kSmoothingSeconds = 3.0;
    double sample = 0;
    uint64_t remaining = 0;
    bool sizesKnown = true;
    bool transferring = false;
    active_ = false;
    for (const DownloadInfo& d : downloads) {
        if (d.state != State::Queued && !isActive(d.state)) continue;
        active_ = active_ || isActive(d.state);
        transferring = transferring || d.state == State::Downloading || d.state == State::Probing;
        if (d.state == State::Downloading) sample += d.bytesPerSecond;
        if (d.bytesTotal == 0) sizesKnown = false;
        else if (d.bytesTotal > d.bytesDone) remaining += d.bytesTotal - d.bytesDone;
    }
    if (!transferring) {
        // Idle, or every transfer is waiting for the network: no meaningful speed.
        speed_ = 0;
        eta_ = -1;
        return;
    }
    if (speed_ <= 0) speed_ = sample;
    else speed_ += (sample - speed_) * (1.0 - std::exp(-std::max(dtSeconds, 0.0) / kSmoothingSeconds));
    eta_ = speed_ > 1 && sizesKnown ? static_cast<double>(remaining) / speed_ : -1;
}

std::string makeKey(std::string_view serverId, std::string_view itemId) {
    std::string k;
    k.reserve(serverId.size() + itemId.size() + 1);
    k.append(serverId);
    k += ':';
    k.append(itemId);
    return k;
}

}  // namespace karafin::downloads
