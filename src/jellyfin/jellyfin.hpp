#pragma once

// Jellyfin module umbrella header. See src/jellyfin/README.md for the API guide.
//
// Depends on core only (no gfx/ui), so player/downloads/screens can all use it.
// Threading: Call<T>::then() delivers on the main thread; Call<T>::wait() blocks (worker threads).

#include <string>

#include "jellyfin/accounts.hpp"
#include "jellyfin/call.hpp"
#include "jellyfin/client.hpp"
#include "jellyfin/device_profile.hpp"
#include "jellyfin/discovery.hpp"
#include "jellyfin/error.hpp"
#include "jellyfin/extras.hpp"
#include "jellyfin/models.hpp"
#include "jellyfin/playback.hpp"
#include "jellyfin/query.hpp"
#include "jellyfin/report_queue.hpp"
#include "jellyfin/url.hpp"

namespace karafin::jellyfin {

// Offline self-test: builds the DeviceProfile and runs parsers / URL builders / plan resolution
// against JSON fixtures embedded in the code. No network. Logs each failure under tag
// "jellyfin-test"; returns true when everything passed. `summary` (optional) gets one line.
bool selfTest(std::string* summary = nullptr);

}  // namespace karafin::jellyfin
