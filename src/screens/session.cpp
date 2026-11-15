#include "screens/session.hpp"

#include <memory>

#include "core/log.hpp"
#include "core/platform.hpp"
#include "screens/login_screen.hpp"
#include "screens/main_screen.hpp"
#include "screens/server_screen.hpp"
#include "screens/splash_screen.hpp"
#include "screens/widgets/busy_modal.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

namespace jf = karafin::jellyfin;

namespace {

bool g_offline = false;
bool g_expiredShown = false;

// Open sheets and dialogs hold callbacks into the screens they were opened from; they must not
// outlive a stack reset (e.g. a 401 while a sort sheet is open).
void dismissModals() {
    while (ui::Modal* m = ui::root().topModal()) m->dismiss();
}

void resetStack(std::unique_ptr<ui::Screen> screen) {
    dismissModals();
    ui::nav().resetTo(std::move(screen), ui::Transition::Fade);
}

void installHandlers(const jf::ClientPtr& client) {
    std::weak_ptr<jf::Client> weak = client;
    client->setUnauthorizedHandler([weak] {
        auto c = weak.lock();
        if (!c || jf::currentClient() != c || g_expiredShown) return;
        g_expiredShown = true;
        log::warn("screens", "session revoked; back to sign-in");
        ui::toast("Your session has expired. Please sign in again.", ui::ToastKind::Warning, gfx::Icon::Lock, 4.0f);
        goToLogin(c->serverId());
    });
    client->setConnectivityHandler([weak](bool offline) {
        auto c = weak.lock();
        if (!c || jf::currentClient() != c || g_offline) return;
        if (offline) ui::toast("Server unreachable. Retrying when you browse.", ui::ToastKind::Warning, gfx::Icon::Wifi, 3.5f);
        else ui::toast("Back online", ui::ToastKind::Success, gfx::Icon::Wifi, 2.0f);
    });
}

}  // namespace

void enterApp(jf::ClientPtr client) {
    if (!client) return;
    g_offline = false;
    g_expiredShown = false;
    jf::setCurrentClient(client);
    installHandlers(client);
    jf::AccountStore::instance().markUsed(client->serverId(), client->userId(), platform::switchProfileId());
    resetStack(std::make_unique<MainScreen>(MainMode::Online));
}

void enterOffline(jf::ClientPtr client) {
    g_offline = true;
    if (client) jf::setCurrentClient(client);
    resetStack(std::make_unique<MainScreen>(MainMode::Offline));
}

bool offlineMode() { return g_offline; }

void restartSession() {
    g_offline = false;
    resetStack(std::make_unique<SplashScreen>(false));
}

void goToServerSelect() {
    g_offline = false;
    resetStack(std::make_unique<ServerScreen>());
}

void goToLogin(const std::string& serverId) {
    auto server = jf::AccountStore::instance().server(serverId);
    if (!server) {
        goToServerSelect();
        return;
    }
    g_offline = false;
    resetStack(std::make_unique<LoginScreen>(jf::Client::fromStored(*server)));
}

void signOut() {
    auto client = jf::currentClient();
    if (!client) {
        goToServerSelect();
        return;
    }
    const std::string serverId = client->serverId();
    auto busy = std::make_unique<BusyModal>("Signing out...");
    ui::Ref<BusyModal> busyRef = busy.get();
    ui::present(std::move(busy));
    // logout() clears the token locally even when the server can't be reached.
    client->logout().then([serverId, busyRef](jf::Status) {
        if (BusyModal* b = busyRef.get()) b->finish();
        jf::setCurrentClient(nullptr);
        goToLogin(serverId);
    });
}

}  // namespace karafin::screens
