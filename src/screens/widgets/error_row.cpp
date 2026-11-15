#include "screens/widgets/error_row.hpp"

#include <utility>

#include "ui/anim.hpp"
#include "ui/root.hpp"
#include "ui/theme.hpp"

namespace karafin::screens {

using namespace karafin::ui;

ErrorRow::ErrorRow(std::string title, std::string message, std::function<void()> retry, std::string retryLabel)
    : Box(Axis::Vertical), text_(std::move(message)), onRetry_(std::move(retry)) {
    setPadding({theme::layout::marginX, 0, theme::layout::marginX, 0}).setSpacing(12);
    if (!title.empty()) add<Label>(std::move(title), theme::type::title3, theme::color::textPrimary);
    auto* line = add<Box>(Axis::Horizontal);
    line->setSpacing(14).setAlign(Align::Center);
    line->add<IconView>(gfx::Icon::Warning, theme::color::warning, 22)->setPreferredSize(22, 22);
    message_ = line->add<Label>(text_, theme::type::body, theme::color::textSecondary);
    message_->setMaxLines(2);
    message_->setFlex(1);
    retry_ = line->add<Button>(std::move(retryLabel), gfx::Icon::Refresh, ButtonStyle::Secondary);
    retry_->onActivate = [this] {
        if (busy_ || !onRetry_) return;
        setBusy(true);
        timeline().after(0.0f, onRetry_, lifetime());
    };
}

void ErrorRow::setMessage(std::string message) {
    text_ = std::move(message);
    if (!busy_) message_->setText(text_);
}

void ErrorRow::setBusy(bool busy) {
    busy_ = busy;
    message_->setText(busy ? std::string("Loading\xE2\x80\xA6") : text_);
    retry_->alpha.tweenTo(busy ? 0.5f : 1.0f, theme::motion::normal, ease::outCubic);
}

}  // namespace karafin::screens
