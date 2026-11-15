#include "screens/widgets/quick_connect_modal.hpp"

#include <algorithm>

#include "gfx/text.hpp"
#include "screens/common.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kCardW = 600;
constexpr float kCardH = 420;
constexpr float kBoxW = 60;
constexpr float kBoxH = 78;
constexpr float kBoxGap = 12;
const gfx::TextStyle kDigit{42, gfx::FontWeight::Display, 1.0f, 0.0f};
}  // namespace

QuickConnectModal::QuickConnectModal(jf::ClientPtr client, std::function<void()> onSuccess)
    : client_(std::move(client)), onSuccess_(std::move(onSuccess)) {
    setDim(0.7f);
    setDismissOnBack(false);
    setDismissOnBackdropTap(false);
    info_ = add<Label>("On a phone or computer signed in to Jellyfin, open your profile, choose Quick Connect and "
                       "enter this code.",
                       theme::type::body, theme::color::textSecondary);
    info_->setAlign(gfx::TextAlign::Center);
    info_->setMaxLines(3);
    spinner_ = add<Spinner>(22, theme::color::accentBright);
    status_ = add<Label>("Requesting a code...", theme::type::callout, theme::color::textSecondary);
    status_->setAlign(gfx::TextAlign::Center);
    status_->setMaxLines(2);
    retry_ = add<Button>("Try again", gfx::Icon::Refresh, ButtonStyle::Primary);
    cancel_ = add<Button>("Cancel", gfx::Icon::None, ButtonStyle::Secondary);
    retry_->onActivate = [this] { timeline().after(0.0f, [this] { start(); }, lifetime()); };
    cancel_->onActivate = [this] { close(); };
    retry_->setVisible(false);
    setRememberedFocus(cancel_);
    start();
}

QuickConnectModal::~QuickConnectModal() {
    if (flow_) flow_->cancel();
}

void QuickConnectModal::start() {
    if (flow_) flow_->cancel();
    code_.clear();
    error_.clear();
    codeIn_.set(0.0f);
    retry_->setVisible(false);
    setNeedsLayout();
    spinner_->setVisible(true);
    status_->setText("Requesting a code...");
    status_->color.set(theme::color::textSecondary);
    focus().setFocus(cancel_);
    flow_ = jf::QuickConnectFlow::start(
        client_,
        guard([this](const std::string& code) {
            code_ = code;
            codeIn_.springTo(1.0f, theme::motion::bouncy);
            status_->setText("Waiting for approval...");
        }),
        guard([this](jf::Result<jf::AuthResult> r) {
            if (r) {
                auto fn = std::move(onSuccess_);
                onSuccess_ = nullptr;
                dismiss();
                if (fn) fn();
                return;
            }
            error_ = errorMessage(r.error);
            spinner_->setVisible(false);
            status_->setText(error_);
            status_->color.set(theme::color::danger);
            retry_->setVisible(true);
            focus().setFocus(retry_);
            setNeedsLayout();
        }));
}

void QuickConnectModal::close() {
    if (flow_) flow_->cancel();
    if (!dismissing()) dismiss();
}

bool QuickConnectModal::onInput(const InputEvent& e) {
    if (e.button == Key::B && e.pressed()) {
        close();
        return true;
    }
    return Modal::onInput(e);
}

void QuickConnectModal::layout() {
    card_ = {(width() - kCardW) * 0.5f, (height() - kCardH) * 0.5f, kCardW, kCardH};
    info_->setFrame({card_.x + 48, card_.y + 78, kCardW - 96, 72});
    const float sy = card_.y + 270;
    status_->setFrame({card_.x + 40, sy + 34, kCardW - 80, 44});
    spinner_->setFrame({card_.center().x - 11, sy, 22, 22});
    const float bw = 150, gap = 14;
    const bool both = retry_->visible();
    const float total = both ? bw * 2 + gap : bw;
    float x = card_.center().x - total * 0.5f;
    if (both) {
        cancel_->setFrame({x, card_.bottom() - 70, bw, theme::button::height});
        retry_->setFrame({x + bw + gap, card_.bottom() - 70, bw, theme::button::height});
    } else {
        cancel_->setFrame({x, card_.bottom() - 70, bw, theme::button::height});
    }
}

void QuickConnectModal::update(float dt) {
    Modal::update(dt);
    codeIn_.update(dt);
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    for (View* v : {static_cast<View*>(info_), static_cast<View*>(status_), static_cast<View*>(spinner_),
                    static_cast<View*>(retry_), static_cast<View*>(cancel_)})
        v->alpha.set(a);
}

void QuickConnectModal::draw(Renderer& r) {
    using namespace theme;
    const float a = std::clamp(appear(), 0.0f, 1.0f);
    r.pushTransform(gfx::Affine::translate(0, (1.0f - a) * 16.0f) * gfx::Affine::scaleAround(0.94f + 0.06f * a, card_.center()));
    r.pushAlpha(a);
    r.drawShadow(card_, radius::xl, 60, Color::black(0.65f), {0, 24});
    r.fillRoundRect(card_, radius::xl, gfx::Gradient::vertical(Color::hex(0x23232F), Color::hex(0x1A1A24)));
    r.strokeRoundRect(card_, radius::xl, 1, color::stroke);
    auto title = gfx::text::layout("Quick Connect", type::title2);
    r.drawText(*title, {card_.center().x - title->width * 0.5f, card_.y + 34}, color::textPrimary);

    // Code: one box per character, springing in when it arrives.
    const size_t n = code_.empty() ? 6 : code_.size();
    const float total = static_cast<float>(n) * kBoxW + static_cast<float>(n - 1) * kBoxGap;
    float x = card_.center().x - total * 0.5f;
    const float y = card_.y + 166;
    const float c = std::clamp(codeIn_.value(), 0.0f, 1.2f);
    for (size_t i = 0; i < n; ++i) {
        const Rect box{x, y, kBoxW, kBoxH};
        if (code_.empty()) {
            r.fillRoundRect(box, radius::md, Color::white(0.06f));
        } else {
            const float d = std::clamp(c * 1.4f - static_cast<float>(i) * 0.08f, 0.0f, 1.0f);
            r.pushTransform(gfx::Affine::translate(0, (1.0f - d) * 14.0f));
            r.drawShadow(box, radius::md, 16, color::accentGlow.mulAlpha(0.35f * d), {0, 6});
            r.fillRoundRect(box, radius::md, gfx::Gradient::vertical(Color::hex(0x34344A), Color::hex(0x2A2A3C)));
            r.strokeRoundRect(box, radius::md, 1.5f, color::accent.mulAlpha(0.6f * d));
            auto l = gfx::text::layout(code_.substr(i, 1), kDigit);
            const float capH = gfx::text::capHeight(kDigit);
            r.drawText(*l, {box.center().x - l->width * 0.5f, box.center().y - capH * 0.5f - (l->firstBaseline - capH)},
                       color::textPrimary.mulAlpha(d));
            r.popTransform();
        }
        x += kBoxW + kBoxGap;
    }
    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::screens
