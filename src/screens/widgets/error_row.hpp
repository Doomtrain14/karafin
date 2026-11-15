#pragma once

#include <functional>
#include <string>

#include "ui/layout.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::screens {

// Inline error for a single row inside a scrolling page (StatusView is the full-page version):
// optional row title, a warning line and a focusable "Try again" button. Pressing it switches
// the row to a busy state (further presses are ignored) and calls `retry` on the next frame, so
// the callback may replace or remove this row. The owner calls setBusy(false) / setMessage() or
// replaces the row when the retry finishes.
//
//   rows_->place(order, std::make_unique<ErrorRow>("Recently Added", errorMessage(err), [this] { reload(); }));
class ErrorRow : public ui::Box {
public:
    ErrorRow(std::string title, std::string message, std::function<void()> retry,
             std::string retryLabel = "Try again");

    void setMessage(std::string message);
    void setBusy(bool busy);
    bool busy() const { return busy_; }
    ui::Button* retryButton() const { return retry_; }

private:
    ui::Label* message_ = nullptr;
    ui::Button* retry_ = nullptr;
    std::string text_;
    std::function<void()> onRetry_;
    bool busy_ = false;
};

}  // namespace karafin::screens
