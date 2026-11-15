#pragma once

#include <functional>
#include <string>

#include "ui/modal.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::screens {

// Blocking "working..." card (spinner + message). B cancels when `onCancel` is set; otherwise
// the user has to wait. Close it with finish() (no callback).
//
//   auto busy = std::make_unique<BusyModal>("Connecting...");
//   busy_ = busy.get();  ui::present(std::move(busy));  ...  busy_->finish();
class BusyModal : public ui::Modal {
public:
    explicit BusyModal(std::string message, std::function<void()> onCancel = {});

    void setMessage(std::string message) { label_->setText(std::move(message)); }
    void finish();

    bool onInput(const ui::InputEvent& e) override;
    void layout() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    ui::Spinner* spinner_ = nullptr;
    ui::Label* label_ = nullptr;
    ui::Label* hint_ = nullptr;
    std::function<void()> onCancel_;
    ui::Rect card_;
};

}  // namespace karafin::screens
