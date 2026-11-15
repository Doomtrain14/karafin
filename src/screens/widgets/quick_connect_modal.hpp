#pragma once

#include <functional>
#include <memory>
#include <string>

#include "jellyfin/client.hpp"
#include "ui/modal.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::screens {

// Quick Connect: shows the 6-digit code large, polls until another signed-in device approves
// it, then signs in. B / Cancel stops polling; errors offer "Try again".
class QuickConnectModal : public ui::Modal {
public:
    QuickConnectModal(jellyfin::ClientPtr client, std::function<void()> onSuccess);
    ~QuickConnectModal() override;

    bool onInput(const ui::InputEvent& e) override;
    void layout() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    void start();
    void close();

    jellyfin::ClientPtr client_;
    std::function<void()> onSuccess_;
    std::shared_ptr<jellyfin::QuickConnectFlow> flow_;
    std::string code_;
    std::string error_;
    ui::Rect card_;
    ui::Label* info_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::Spinner* spinner_ = nullptr;
    ui::Button* retry_ = nullptr;
    ui::Button* cancel_ = nullptr;
    ui::Animated<float> codeIn_{0.0f};
};

}  // namespace karafin::screens
