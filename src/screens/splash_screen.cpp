#include "screens/splash_screen.hpp"

#include <algorithm>
#include <cmath>

#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/platform.hpp"
#include "core/settings.hpp"
#include "downloads/manager.hpp"
#include "gfx/image.hpp"
#include "gfx/text.hpp"
#include "screens/common.hpp"
#include "screens/session.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kMinColdTime = 1.3f;
constexpr float kMinWarmTime = 0.35f;
const gfx::TextStyle kWordmark{42, gfx::FontWeight::SemiBold, 1.0f, 0.6f};
// Corner radius of the rounded-square mark, as a fraction of its side (tools/gen_icon.py).
constexpr float kIconRadius = 0.20f;
constexpr float kIcon = 168.0f;
}  // namespace

SplashScreen::SplashScreen(bool coldStart) : coldStart_(coldStart) {
    setShowsHintBar(false);
    spinner_ = add<Spinner>(28, theme::color::accentBright);
    spinner_->setFrame({theme::layout::screenWidth * 0.5f - 14, 470, 28, 28});
    spinner_->alpha.set(0.0f);
    statusText_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
    statusText_->setAlign(gfx::TextAlign::Center);
    statusText_->setFrame({240, 512, 800, 24});
    statusText_->alpha.set(0.0f);
    problem_ = add<StatusView>();
    problem_->setFrame({0, 300, theme::layout::screenWidth, 380});
    logo_.springTo(1.0f, SpringSpec{0.9f, 0.78f});
    gfx::ImageRequest req;
    req.url = paths::romfs("icon.jpg");
    // Decode at the drawn size: the area-averaged shrink stays smooth, while GL minifying the
    // full-size texture (no mipmaps) leaves jagged edges.
    req.width = req.height = static_cast<int>(std::ceil(kIcon * ui::displayScale()));
    req.fit = gfx::ImageFit::Fill;
    icon_ = gfx::ImageCache::instance().load(req);
}

SplashScreen::~SplashScreen() { token_.cancel(); }

void SplashScreen::onShow() {
    if (started_) return;
    started_ = true;
    timeline().after(coldStart_ ? 0.25f : 0.0f, [this] { restore(); }, lifetime());
}

void SplashScreen::restore() {
    token_.cancel();
    token_.reset();
    outcome_ = Outcome::Pending;
    problem_->hide();
    setShowsHintBar(false);
    lift_.springTo(0.0f, theme::motion::gentle);
    auto last = jf::AccountStore::instance().lastSession();
    if (!last) {
        outcome_ = Outcome::Servers;
        maybeNavigate();
        return;
    }
    serverId_ = last->server.id;
    serverName_ = last->server.name.empty() ? last->server.url : last->server.name;
    statusText_->setText("Connecting to " + serverName_ + "...");
    spinner_->alpha.tweenTo(1.0f, 0.4f, ease::outCubic);
    statusText_->alpha.tweenTo(1.0f, 0.4f, ease::outCubic);
    client_ = jf::Client::fromStored(last->server, &last->user);
    client_->validateSession().then(
        [this](jf::Result<jf::UserDto> r) {
            if (r) {
                outcome_ = Outcome::Home;
            } else if (r.error.kind == jf::ErrorKind::Unauthorized) {
                outcome_ = Outcome::Login;
            } else if (r.error.isConnectivity()) {
                outcome_ = Outcome::Offline;
            } else {
                log::warn("screens", "session restore failed: {}", r.error.describe());
                outcome_ = Outcome::Error;
                statusText_->setText(errorMessage(r.error));
            }
            maybeNavigate();
        },
        token_);
}

void SplashScreen::maybeNavigate() {
    if (navigated_ || outcome_ == Outcome::Pending || dialogOpen_) return;
    if (t_ < (coldStart_ ? kMinColdTime : kMinWarmTime)) return;
    if (coldStart_ && !warningChecked_ && platform::isAppletMode()) return;  // warning first
    switch (outcome_) {
        case Outcome::Home:
            navigated_ = true;
            enterApp(client_);
            break;
        case Outcome::Login:
            navigated_ = true;
            goToLogin(serverId_);
            break;
        case Outcome::Servers:
            navigated_ = true;
            goToServerSelect();
            break;
        case Outcome::Offline:
            showProblem("Can't reach " + serverName_,
                        "Check that the console is online and the server is running. Your downloads are still "
                        "available offline.",
                        true);
            break;
        case Outcome::Error:
            showProblem("Couldn't sign in", statusText_->text(), false);
            break;
        case Outcome::Pending: break;
    }
}

void SplashScreen::showProblem(const std::string& title, const std::string& message, bool offline) {
    if (problem_->showing()) return;
    spinner_->alpha.tweenTo(0.0f, 0.2f);
    statusText_->alpha.tweenTo(0.0f, 0.2f);
    lift_.springTo(1.0f, theme::motion::gentle);
    setShowsHintBar(true);
    std::vector<StatusAction> actions;
    actions.push_back({"Try again", gfx::Icon::Refresh, [this] { restore(); }, !offline});
    // A server error (not just "unreachable") still leaves the downloads playable.
    if (offline || downloads::DownloadManager::instance().storage().completedCount > 0) {
        actions.push_back({"Offline downloads", gfx::Icon::Download, [this] {
                               navigated_ = true;
                               enterOffline(client_);
                           },
                           offline});
    }
    actions.push_back({"Change server", gfx::Icon::Server, [this] {
                           navigated_ = true;
                           goToServerSelect();
                       }});
    problem_->showMessage(offline ? gfx::Icon::Wifi : gfx::Icon::Warning, title, message, std::move(actions),
                          offline ? theme::color::warning : theme::color::danger);
    problem_->focusAction();
}

void SplashScreen::showAppletWarning() {
    warningChecked_ = true;
    if (Settings::instance().get<bool>("ui.applet_warning_hidden", false)) return;
    DialogSpec spec;
    spec.title = "Running in applet mode";
    spec.message =
        "Karafin was opened from the Album, which leaves much less memory for video. Playback may stutter or "
        "fail. For the best experience, hold R while launching any game to start Karafin with full memory "
        "(title override).";
    spec.icon = gfx::Icon::Warning;
    spec.iconColor = theme::color::warning;
    spec.buttons = {{"Don't show again", ButtonStyle::Secondary,
                     [] { Settings::instance().set("ui.applet_warning_hidden", true); }},
                    {"Continue", ButtonStyle::Primary, {}}};
    spec.cancelIndex = 1;
    spec.defaultIndex = 1;
    auto dialog = std::make_unique<Dialog>(std::move(spec));
    dialog->onDismissed = guard([this] {
        dialogOpen_ = false;
        maybeNavigate();
    });
    dialogOpen_ = true;
    present(std::move(dialog));
}

void SplashScreen::update(float dt) {
    t_ += dt;
    logo_.update(dt);
    lift_.update(dt);
    if (coldStart_ && !warningChecked_ && t_ > 0.7f) {
        if (platform::isAppletMode()) showAppletWarning();
        else warningChecked_ = true;
    }
    maybeNavigate();
    Screen::update(dt);
}

void SplashScreen::draw(Renderer& r) {
    Screen::draw(r);
    using namespace theme;
    const Rect b = bounds();
    // Slowly drifting ambient glows.
    const float t = t_;
    const float a = std::clamp(logo_.value(), 0.0f, 1.0f);
    const Vec2 g1{b.w * 0.32f + std::sin(t * 0.35f) * 60.0f, b.h * 0.38f + std::cos(t * 0.27f) * 30.0f};
    const Vec2 g2{b.w * 0.70f + std::cos(t * 0.31f) * 70.0f, b.h * 0.58f + std::sin(t * 0.22f) * 40.0f};
    r.drawShadow({g1.x - 120, g1.y - 120, 240, 240}, 120, 260, color::accentDeep.withAlpha(0.35f * a));
    r.drawShadow({g2.x - 110, g2.y - 110, 220, 220}, 110, 240, color::accent.withAlpha(0.22f * a));

    const float lift = std::clamp(lift_.value(), -0.2f, 1.2f);
    const float s = 0.86f + 0.14f * logo_.value();
    const Vec2 center{b.w * 0.5f, 300.0f - 120.0f * lift};
    r.pushTransform(gfx::Affine::scaleAround(s, center));
    r.pushAlpha(a);
    // Same mark as the home-menu icon, sitting where the drawn jellyfish used to.
    const Rect icon{center.x - kIcon * 0.5f, center.y - 176.0f, kIcon, kIcon};
    const float radius = kIcon * kIconRadius;
    if (const gfx::Texture* tex = icon_ ? icon_->texture() : nullptr) {
        r.drawShadow(icon, radius, 28, gfx::Color(0, 0, 0, 0.45f), {0, 10});
        r.drawImage(*tex, icon, {.fit = gfx::ImageFit::Fill, .radius = radius});
    }
    auto layout = gfx::text::layout("Karafin", kWordmark);
    const Vec2 pos{center.x - layout->width * 0.5f, icon.bottom() + 22};
    r.drawText(*layout, pos, color::textPrimary);
    const float tagA = std::clamp((t_ - 0.45f) / 0.5f, 0.0f, 1.0f);
    auto tag = gfx::text::layout("Jellyfin for Nintendo Switch", type::headline);
    r.drawText(*tag, {center.x - tag->width * 0.5f, pos.y + layout->height + 6}, color::textSecondary.mulAlpha(tagA));
    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::screens
