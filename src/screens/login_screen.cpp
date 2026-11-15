#include "screens/login_screen.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "downloads/manager.hpp"
#include "gfx/text.hpp"
#include "screens/common.hpp"
#include "screens/keyboard.hpp"
#include "screens/server_screen.hpp"
#include "screens/session.hpp"
#include "screens/widgets/avatar_tile.hpp"
#include "screens/widgets/quick_connect_modal.hpp"
#include "ui/root.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kUsersY = 238;
constexpr float kActionsY = 500;
constexpr size_t kMaxTiles = 6;  // + "Other user": 7 tiles fit the screen width
}  // namespace

LoginScreen::LoginScreen(jf::ClientPtr client) : client_(std::move(client)) {
    const auto info = client_->serverInfo();
    serverName_ = info.name.empty() ? client_->baseUrl() : info.name;

    title_ = add<Label>("Who's watching?", theme::type::largeTitle);
    title_->setAlign(gfx::TextAlign::Center);
    title_->setFrame({0, 120, theme::layout::screenWidth, 44});
    subtitle_ = add<Label>(joinDots({serverName_, client_->baseUrl()}), theme::type::callout, theme::color::textSecondary);
    subtitle_->setAlign(gfx::TextAlign::Center);
    subtitle_->setFrame({0, 172, theme::layout::screenWidth, 22});

    users_ = add<Box>(Axis::Horizontal);
    users_->setSpacing(26).setAlign(Align::Start);
    users_->setRememberFocus(true);
    actions_ = add<Box>(Axis::Horizontal);
    actions_->setSpacing(14);
    actions_->setRememberFocus(true);

    spinner_ = add<Spinner>(36, theme::color::accentBright);
    spinner_->setFrame({theme::layout::screenWidth * 0.5f - 18, 320, 36, 36});
    status_ = add<StatusView>();
    status_->setFrame({0, 210, theme::layout::screenWidth, 420});
    load();
}

LoginScreen::~LoginScreen() {
    token_.cancel();
    authToken_.cancel();
}

void LoginScreen::load() {
    token_.cancel();
    token_.reset();
    status_->hide();
    users_->clearChildren();
    actions_->clearChildren();
    spinner_->setVisible(true);
    quickConnect_ = false;
    client_->quickConnectEnabled().then(
        [this](jf::Result<bool> r) {
            quickConnect_ = r && *r;
            if (!users_->children().empty()) buildActions();
        },
        token_);
    client_->publicUsers().then(
        [this](jf::Result<std::vector<jf::UserDto>> r) {
            spinner_->setVisible(false);
            if (!r && r.error.isConnectivity()) {
                std::vector<StatusAction> extra;
                extra.push_back({"Change server", gfx::Icon::Server, [] { goToServerSelect(); }});
                if (downloads::DownloadManager::instance().storage().completedCount > 0)
                    extra.push_back({"Offline downloads", gfx::Icon::Download, [] { enterOffline(nullptr); }});
                status_->showError(errorMessage(r.error), [this] { load(); }, std::move(extra),
                                   "Can't reach " + serverName_);
                status_->focusAction();
                return;
            }
            if (r) publicUsers_ = *r;
            buildUsers();
        },
        token_);
}

void LoginScreen::buildUsers() {
    const std::vector<jf::UserDto>& pub = publicUsers_;
    users_->clearChildren();
    const auto server = jf::AccountStore::instance().server(client_->serverId());
    std::vector<std::string> shownIds;

    if (server) {
        for (const auto& u : server->users) {
            if (!u.hasToken() || users_->childCount() >= kMaxTiles) continue;
            const std::string url = client_->userImageUrl(u.id, u.primaryImageTag, {.maxWidth = 240});
            auto* tile = users_->add<AvatarTile>(u.name, url);
            tile->setMarked(true);
            tile->setCaption("Signed in");
            tile->setHints({{Key::Y, "Forget"}});
            const jf::StoredUser user = u;
            tile->onActivate = [this, user] { signInStored(user); };
            tile->onSecondary = [this, user] {
                present(Dialog::confirm("Forget " + user.name + "?", "The saved sign-in on this console will be removed.",
                                        "Forget", "Cancel", guard([this, id = user.id](bool ok) {
                                            if (!ok) return;
                                            jf::AccountStore::instance().removeUser(client_->serverId(), id);
                                            timeline().after(0.0f, [this] { buildUsers(); }, lifetime());
                                        })));
            };
            shownIds.push_back(u.id);
        }
    }
    for (const auto& u : pub) {
        if (users_->childCount() >= kMaxTiles) break;
        if (std::find(shownIds.begin(), shownIds.end(), u.id) != shownIds.end()) continue;
        const std::string url = client_->userImageUrl(u.id, u.primaryImageTag, {.maxWidth = 240});
        auto* tile = users_->add<AvatarTile>(u.name, url);
        const std::string name = u.name;
        const bool needsPassword = u.hasPassword || u.hasConfiguredPassword;
        tile->onActivate = [this, name, needsPassword] {
            if (needsPassword) askPassword(name, true);
            else authenticate(name, "");
        };
        shownIds.push_back(u.id);
    }
    auto* other = users_->add<AvatarTile>("Other user", "", gfx::Icon::Plus);
    other->setCaption("Username and password");
    other->onActivate = [this] { askUsername(); };

    buildActions();
    const Vec2 um = users_->measure({theme::layout::screenWidth, 300});
    users_->setFrame({std::round((theme::layout::screenWidth - um.x) * 0.5f), kUsersY, um.x, um.y});
    users_->alpha.set(0.0f);
    users_->alpha.tweenTo(1.0f, 0.4f, ease::outCubic);
    users_->offset.set({0, 16});
    users_->offset.springTo({0, 0}, theme::motion::gentle);
    if (!actions_->hasFocusWithin())
        if (View* first = users_->childAt(0)) focus().setFocus(first);
}

void LoginScreen::buildActions() {
    const bool hadFocus = actions_->hasFocusWithin();
    actions_->clearChildren();
    if (quickConnect_) {
        auto* qc = actions_->add<Button>("Quick Connect", gfx::Icon::Lock, ButtonStyle::Secondary);
        qc->onActivate = [this] { startQuickConnect(); };
    }
    auto* change = actions_->add<Button>("Change server", gfx::Icon::Server, ButtonStyle::Ghost);
    change->onActivate = [] { goToServerSelect(); };
    const Vec2 am = actions_->measure({theme::layout::screenWidth, 60});
    actions_->setFrame({std::round((theme::layout::screenWidth - am.x) * 0.5f), kActionsY, am.x, am.y});
    if (hadFocus) focus().setFocus(actions_->childAt(0));
}

void LoginScreen::showBusy(const std::string& message) {
    auto busy = std::make_unique<BusyModal>(message, [this] { authToken_.cancel(); });
    busy_ = busy.get();
    present(std::move(busy));
}

void LoginScreen::hideBusy() {
    if (BusyModal* b = busy_.get()) b->finish();
    busy_.reset();
}

void LoginScreen::signInStored(const jf::StoredUser& user) {
    const auto server = jf::AccountStore::instance().server(client_->serverId());
    if (!server) return;
    authToken_.cancel();
    authToken_.reset();
    showBusy("Signing in as " + user.name + "...");
    auto c = jf::Client::fromStored(*server, &user);
    const std::string name = user.name;
    c->validateSession().then(
        [this, c, name](jf::Result<jf::UserDto> r) {
            hideBusy();
            if (r) {
                succeed(c);
                return;
            }
            if (r.error.kind == jf::ErrorKind::Unauthorized) {
                toast("Your saved sign-in expired. Enter the password again.", ToastKind::Warning, gfx::Icon::Lock, 3.5f);
                askPassword(name, true);
                return;
            }
            present(Dialog::alert("Can't sign in", errorMessage(r.error)));
        },
        authToken_);
}

void LoginScreen::askUsername() {
    KeyboardOptions opts;
    opts.header = "Sign in to " + serverName_;
    opts.guide = "Username";
    opts.okLabel = "Next";
    opts.maxLength = 128;
    requestText(std::move(opts), guard([this](std::string name) { askPassword(name, true); }));
}

void LoginScreen::askPassword(const std::string& username, bool allowEmpty) {
    KeyboardOptions opts;
    opts.header = "Password";
    opts.sub = "for " + username;
    opts.guide = "Password";
    opts.okLabel = "Sign in";
    opts.kind = KeyboardKind::Password;
    opts.maxLength = 256;
    opts.allowEmpty = allowEmpty;
    requestText(std::move(opts), guard([this, username](std::string pw) { authenticate(username, pw); }));
}

void LoginScreen::authenticate(const std::string& username, const std::string& password) {
    authToken_.cancel();
    authToken_.reset();
    showBusy("Signing in as " + username + "...");
    client_->authenticateByName(username, password).then(
        [this, username](jf::Result<jf::AuthResult> r) {
            hideBusy();
            if (r) {
                succeed(client_);
                return;
            }
            DialogSpec spec;
            spec.title = "Sign-in failed";
            spec.message = errorMessage(r.error);
            spec.icon = gfx::Icon::Lock;
            spec.iconColor = theme::color::danger;
            spec.buttons = {{"Cancel", ButtonStyle::Secondary, {}},
                            {"Try again", ButtonStyle::Primary, guard([this, username] { askPassword(username, true); })}};
            spec.cancelIndex = 0;
            spec.defaultIndex = 1;
            present(std::make_unique<Dialog>(std::move(spec)));
        },
        authToken_);
}

void LoginScreen::startQuickConnect() {
    present(std::make_unique<QuickConnectModal>(client_, guard([this] { succeed(client_); })));
}

void LoginScreen::succeed(jf::ClientPtr client) { enterApp(std::move(client)); }

void LoginScreen::onShow() {
    // At the root (session expired, switch user) B goes to the server list; otherwise the
    // automatic "Back" hint applies.
    setHints(nav().depth() > 1 ? std::vector<Hint>{} : std::vector<Hint>{{Key::B, "Servers"}});
}

bool LoginScreen::onBack() {
    if (nav().depth() > 1) return false;
    nav().replace(std::make_unique<ServerScreen>(), Transition::Fade);
    return true;
}

void LoginScreen::update(float dt) {
    t_ += dt;
    Screen::update(dt);
}

void LoginScreen::draw(Renderer& r) {
    Screen::draw(r);
    using namespace theme;
    const Rect b = bounds();
    const Vec2 g{b.w * 0.5f + std::sin(t_ * 0.3f) * 80.0f, 330 + std::cos(t_ * 0.23f) * 30.0f};
    r.drawShadow({g.x - 260, g.y - 110, 520, 220}, 110, 300, color::accentDeep.withAlpha(0.22f));
}

}  // namespace karafin::screens
