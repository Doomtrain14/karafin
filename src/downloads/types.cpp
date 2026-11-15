#include "downloads/types.hpp"

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

std::string makeKey(std::string_view serverId, std::string_view itemId) {
    std::string k;
    k.reserve(serverId.size() + itemId.size() + 1);
    k.append(serverId);
    k += ':';
    k.append(itemId);
    return k;
}

}  // namespace karafin::downloads
