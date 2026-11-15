#pragma once

// Downloads module umbrella header. See src/downloads/README.md for the API guide.
//
// Depends on core and jellyfin (no gfx/ui). DownloadManager owns the persistent queue
// (sdmc:/switch/karafin/downloads/queue.json), the transfers (dedicated dl-N threads, HTTP Range
// resume into <file>.part) and the offline library. Events reach the UI on the main thread.

#include <string>

#include "downloads/manager.hpp"
#include "downloads/types.hpp"

namespace karafin::downloads {

// Offline self-test: state machine, queue operations, queue.json round trip and restore rules,
// helpers (extension, Content-Range, backoff, progress patching, throttle math) and the
// .part writer (in the cache dir). No network. Logs failures under "downloads-test"; returns
// true when everything passed. `summary` (optional) gets one line.
bool selfTest(std::string* summary = nullptr);

}  // namespace karafin::downloads
