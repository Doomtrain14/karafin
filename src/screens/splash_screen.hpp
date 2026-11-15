#pragma once

#include <cstdint>
#include <string>

#include "core/cancel.hpp"
#include "gfx/image.hpp"
#include "jellyfin/client.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/anim.hpp"
#include "ui/screen.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::screens {

// Animated logo while the last session is restored (fromStored + validateSession):
// success -> Home, 401 -> sign-in, unreachable -> offer offline downloads / retry / change
// server, nothing saved -> server selection. Shows the applet-mode memory warning once. A profile that
// asks for its lock code asks before Home; backing out goes to that server's sign-in page.
class SplashScreen : public ui::Screen {
public:
    explicit SplashScreen(bool coldStart = true);
    ~SplashScreen() override;

    void onShow() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    enum class Outcome : uint8_t { Pending, Home, Login, Servers, Offline, Error };

    void restore();
    void maybeNavigate();
    void showProblem(const std::string& title, const std::string& message, bool offline);
    void showAppletWarning();

    bool coldStart_;
    float t_ = 0;
    Outcome outcome_ = Outcome::Pending;
    jellyfin::ClientPtr client_;
    std::string serverId_;
    std::string userId_;
    std::string userName_;
    std::string serverName_;
    bool dialogOpen_ = false;
    bool warningChecked_ = false;
    bool navigated_ = false;
    bool started_ = false;
    ui::Animated<float> logo_{0.0f};
    ui::Animated<float> lift_{0.0f};
    gfx::ImagePtr icon_;
    ui::Spinner* spinner_ = nullptr;
    ui::Label* statusText_ = nullptr;
    StatusView* problem_ = nullptr;
    CancelToken token_;
};

}  // namespace karafin::screens
