#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/client.hpp"
#include "screens/widgets/busy_modal.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/layout.hpp"
#include "ui/screen.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::screens {

// "Who's watching?": remembered accounts (instant sign-in), public users (password via the
// system keyboard), "Other user" (username + password), Quick Connect and change server.
class LoginScreen : public ui::Screen {
public:
    // `client`: an unauthenticated client for the server.
    explicit LoginScreen(jellyfin::ClientPtr client);
    ~LoginScreen() override;

    void onShow() override;
    bool onBack() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    void load();
    void buildUsers();
    void buildActions();
    void signInStored(const jellyfin::StoredUser& user);
    void askPassword(const std::string& username, bool allowEmpty);
    void askUsername();
    void authenticate(const std::string& username, const std::string& password);
    void startQuickConnect();
    void showBusy(const std::string& message);
    void hideBusy();
    void succeed(jellyfin::ClientPtr client);

    jellyfin::ClientPtr client_;
    std::string serverName_;
    std::vector<jellyfin::UserDto> publicUsers_;
    ui::Label* title_ = nullptr;
    ui::Label* subtitle_ = nullptr;
    ui::Box* users_ = nullptr;
    ui::Box* actions_ = nullptr;
    ui::Spinner* spinner_ = nullptr;
    StatusView* status_ = nullptr;
    ui::Ref<BusyModal> busy_;
    bool quickConnect_ = false;
    float t_ = 0;
    CancelToken token_;
    CancelToken authToken_;
};

}  // namespace karafin::screens
