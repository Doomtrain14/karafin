#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gfx/icons.hpp"
#include "ui/layout.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::screens {

struct StatusAction {
    std::string label;
    gfx::Icon icon = gfx::Icon::None;
    std::function<void()> onPress;
    bool primary = false;
};

// Centered loading / empty / error state with optional action buttons. Fades in on change.
// Action callbacks run on the next frame, so they may freely rebuild the view that owns this.
//
//   status_->showError(errorMessage(r.error), [this] { reload(); });
//   status_->showEmpty(gfx::Icon::Download, "No downloads yet", "Open a movie and press Download.");
//   status_->hide();
class StatusView : public ui::View {
public:
    StatusView();

    void showLoading(std::string text = {});
    void showMessage(gfx::Icon icon, std::string title, std::string message, std::vector<StatusAction> actions = {},
                     ui::Color iconColor = ui::theme::color::textSecondary);
    void showEmpty(gfx::Icon icon, std::string title, std::string message, std::vector<StatusAction> actions = {});
    // Error with a "Try again" button (when `retry` is set) plus optional extra actions.
    void showError(std::string message, std::function<void()> retry, std::vector<StatusAction> extra = {},
                   std::string title = "Something went wrong");
    void hide();
    bool showing() const { return visible(); }
    // Focuses the primary (or first) action button, if any.
    bool focusAction();

    void layout() override;
    void update(float dt) override;

private:
    void reset();
    void setActions(std::vector<StatusAction> actions);

    ui::Box* box_ = nullptr;
    ui::IconView* icon_ = nullptr;
    ui::Spinner* spinner_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Label* message_ = nullptr;
    ui::Box* buttons_ = nullptr;
    std::vector<ui::Button*> actionButtons_;
    size_t primaryIndex_ = 0;
    ui::Animated<float> appear_{0.0f};
};

}  // namespace karafin::screens
