#pragma once

#include <functional>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/call.hpp"
#include "jellyfin/models.hpp"

namespace karafin::jellyfin {

// LAN discovery: broadcasts "who is JellyfinServer?" to UDP 7359 and collects replies for
// `timeoutMs`. Results are de-duplicated by server id.
//
// Async: runs on its own short-lived thread; `cb` runs on the main thread (skipped if `token`
// was cancelled). Starting a new discovery cancels a running one.
void discoverServers(std::function<void(Result<std::vector<DiscoveredServer>>)> cb, int timeoutMs = 2500,
                     CancelToken token = {});
// Blocking variant (worker / dedicated threads).
Result<std::vector<DiscoveredServer>> discoverServersBlocking(int timeoutMs = 2500, CancelToken token = {});

}  // namespace karafin::jellyfin
