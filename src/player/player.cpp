#include "player/player.hpp"

#include <memory>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/platform.hpp"
#include "player/player_screen.hpp"
#include "ui/root.hpp"

namespace karafin::player {

void play(const PlayRequest& req) {
    if (!main_thread::isCurrent()) {
        main_thread::post([req] { play(req); });
        return;
    }
    if (!ui::Root::exists() || req.itemId.empty()) {
        log::warn("player", "play() ignored (item '{}', ui {})", req.itemId, ui::Root::exists());
        return;
    }
    if (platform::isAppletMode())
        ui::toast("Applet mode has little memory: launch Karafin via a game (hold R) for reliable playback",
                  ui::ToastKind::Warning, gfx::Icon::Warning, 5.0f);
    if (dynamic_cast<PlayerScreen*>(ui::nav().top())) {
        ui::nav().replace(std::make_unique<PlayerScreen>(req), ui::Transition::Fade);
        return;
    }
    ui::nav().push(std::make_unique<PlayerScreen>(req), ui::Transition::Zoom);
}

void shutdown(int timeoutMs) {
    if (!main_thread::isCurrent()) {
        log::error("player", "shutdown() must run on the main thread");
        return;
    }
    PlayerScreen::shutdownAll(timeoutMs);
}

}  // namespace karafin::player
