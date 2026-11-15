#pragma once

#include <string>

#include "jellyfin/client.hpp"

// App-level navigation between the session states:
//   Splash -> ServerSelect -> Login -> Main (online)
//                  \-> Main (offline: Downloads + Settings only)
namespace karafin::screens {

// Makes `client` the current session (401 / connectivity handlers, account "last used") and
// resets the stack to the main screen.
void enterApp(jellyfin::ClientPtr client);
// Offline mode: only Downloads and Settings. `client` (optional) is kept as the current client so
// queued progress reports and downloads can resume once the server is reachable again.
void enterOffline(jellyfin::ClientPtr client = nullptr);
bool offlineMode();

// Back to the start: re-runs the splash flow (restore last session).
void restartSession();
void goToServerSelect();
// Sign-in page for a saved server (no user token is used).
void goToLogin(const std::string& serverId);
// Logs the current user out (best effort when offline) and shows the sign-in page.
void signOut();

}  // namespace karafin::screens
