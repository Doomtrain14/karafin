#pragma once

#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "screens/widgets/busy_modal.hpp"
#include "ui/anim.hpp"
#include "ui/screen.hpp"
#include "ui/scroll.hpp"
#include "ui/view.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::screens {

// Server selection: saved servers, servers discovered on the LAN (refreshed while visible), add
// a server by address (system keyboard), remove saved servers (Y), and offline downloads.
class ServerScreen : public ui::Screen {
public:
    ServerScreen();
    ~ServerScreen() override;

    void onShow() override;
    void onHide() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    void rebuild();
    void discover();
    void addServer(const std::string& initial = {});
    void connect(const std::string& address, const std::string& label);

    ui::ScrollView* list_ = nullptr;
    std::vector<jellyfin::DiscoveredServer> discovered_;
    bool discovering_ = false;
    bool searchedOnce_ = false;
    bool visible_ = false;
    float rediscoverIn_ = 0;
    float t_ = 0;
    ui::Ref<BusyModal> busy_;
    CancelToken discoverToken_;
    CancelToken connectToken_;
};

}  // namespace karafin::screens
