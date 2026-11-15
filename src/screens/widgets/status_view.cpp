#include "screens/widgets/status_view.hpp"

#include <algorithm>
#include <limits>

#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;

StatusView::StatusView() {
    box_ = add<Box>(Axis::Vertical);
    box_->setSpacing(10).setAlign(Align::Center);
    spinner_ = box_->add<Spinner>(40, theme::color::accentBright);
    spinner_->setPreferredSize(40, 40);
    icon_ = box_->add<IconView>(gfx::Icon::Info, theme::color::textSecondary, 44);
    icon_->setMargin({0, 0, 0, 6});
    title_ = box_->add<Label>("", theme::type::title2);
    title_->setAlign(gfx::TextAlign::Center);
    title_->setMaxLines(2);
    message_ = box_->add<Label>("", theme::type::body, theme::color::textSecondary);
    message_->setAlign(gfx::TextAlign::Center);
    message_->setMaxLines(5);
    buttons_ = box_->add<Box>(Axis::Horizontal);
    buttons_->setSpacing(14).setAlign(Align::Center);
    buttons_->setMargin({0, 16, 0, 0});
    setVisible(false);
}

void StatusView::reset() {
    setVisible(true);
    appear_.set(0.0f);
    appear_.tweenTo(1.0f, theme::motion::slow, ease::outCubic);
    setNeedsLayout();
}

void StatusView::setActions(std::vector<StatusAction> actions) {
    buttons_->clearChildren();
    actionButtons_.clear();
    primaryIndex_ = 0;
    for (size_t i = 0; i < actions.size(); ++i) {
        auto& a = actions[i];
        auto* b = buttons_->add<Button>(a.label, a.icon, a.primary ? ButtonStyle::Primary : ButtonStyle::Secondary);
        b->setPreferredSize(std::max(150.0f, b->measure({1000, 100}).x), theme::button::height);
        auto fn = a.onPress;
        b->onActivate = [this, fn] {
            if (fn) timeline().after(0.0f, fn, lifetime());
        };
        if (a.primary) primaryIndex_ = i;
        actionButtons_.push_back(b);
    }
    buttons_->setVisible(!actionButtons_.empty());
}

void StatusView::showLoading(std::string text) {
    reset();
    spinner_->setVisible(true);
    icon_->setVisible(false);
    title_->setVisible(false);
    message_->setText(std::move(text));
    message_->setVisible(!message_->text().empty());
    setActions({});
}

void StatusView::showMessage(gfx::Icon icon, std::string title, std::string message, std::vector<StatusAction> actions,
                             Color iconColor) {
    reset();
    spinner_->setVisible(false);
    icon_->setIcon(icon);
    icon_->color.set(iconColor);
    icon_->setVisible(icon != gfx::Icon::None);
    title_->setText(std::move(title));
    title_->setVisible(!title_->text().empty());
    message_->setText(std::move(message));
    message_->setVisible(!message_->text().empty());
    setActions(std::move(actions));
}

void StatusView::showEmpty(gfx::Icon icon, std::string title, std::string message, std::vector<StatusAction> actions) {
    showMessage(icon, std::move(title), std::move(message), std::move(actions), theme::color::textTertiary);
}

void StatusView::showError(std::string message, std::function<void()> retry, std::vector<StatusAction> extra,
                           std::string title) {
    std::vector<StatusAction> actions;
    if (retry) actions.push_back({"Try again", gfx::Icon::Refresh, std::move(retry), true});
    for (auto& e : extra) actions.push_back(std::move(e));
    showMessage(gfx::Icon::Warning, std::move(title), std::move(message), std::move(actions), theme::color::warning);
}

void StatusView::hide() {
    if (!visible()) return;
    setVisible(false);
    buttons_->clearChildren();
    actionButtons_.clear();
}

bool StatusView::focusAction() {
    if (actionButtons_.empty() || !Root::exists()) return false;
    Root::instance().focus().setFocus(actionButtons_[std::min(primaryIndex_, actionButtons_.size() - 1)]);
    return true;
}

void StatusView::layout() {
    const float w = std::min(width() - 48, 560.0f);
    const Vec2 m = box_->measure({w, std::numeric_limits<float>::infinity()});
    box_->setFrame({(width() - w) * 0.5f, std::max(0.0f, (height() - m.y) * 0.5f), w, m.y});
}

void StatusView::update(float dt) {
    appear_.update(dt);
    const float a = std::clamp(appear_.value(), 0.0f, 1.0f);
    box_->alpha.set(a);
    box_->offset.set({0, (1.0f - a) * 14.0f});
    View::update(dt);
}

}  // namespace karafin::screens
