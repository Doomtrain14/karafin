#pragma once

// Public player API. Screens call player::play(); everything else in src/player is internal.
// See src/player/README.md for settings keys and controls.

#include <cstdint>
#include <optional>
#include <string>

namespace karafin::player {

struct PlayRequest {
    std::string itemId;
    std::optional<int64_t> startPositionTicks;  // nullopt = resume from item UserData (ask resume/start-over is done by the caller)
    std::optional<int> audioStreamIndex;        // Jellyfin stream index
    std::optional<int> subtitleStreamIndex;     // Jellyfin stream index, -1 = off
    std::string mediaSourceId;                  // empty = default
    bool offline = false;                       // play from downloads store
};

void play(const PlayRequest& req);  // pushes the player screen onto ui nav (main thread)

// App quit. Main thread, while the UI, DownloadManager and http::Client are still running (call
// it first in the app's onStop). If a player is open: stops mpv, saves local progress (offline
// items) or sends the final /Sessions/Playing/Stopped report, waiting for the server up to
// `timeoutMs` (a report that doesn't get through is persisted in jellyfin::ReportQueue and sent
// on the next launch), releases the platform locks and the download throttle, frees the render
// context and waits for mpv's teardown within the same budget. The screen itself is destroyed
// later by ui::Root::onStop and reports nothing again. No-op without a player.
void shutdown(int timeoutMs = 2000);

}  // namespace karafin::player
