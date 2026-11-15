#include "screens/server_screen.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

#include "core/platform.hpp"
#include "downloads/manager.hpp"
#include "gfx/text.hpp"
#include "jellyfin/client.hpp"
#include "jellyfin/discovery.hpp"
#include "screens/common.hpp"
#include "screens/keyboard.hpp"
#include "screens/login_screen.hpp"
#include "screens/session.hpp"
#include "screens/widgets/action_row.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kListX = 500;
constexpr float kRediscoverSeconds = 8.0f;

std::unique_ptr<Label> heading(const std::string& text) {
    auto l = std::make_unique<Label>(text, theme::type::overline, theme::color::accentBright);
    l->setMargin({4, 18, 0, 2});
    return l;
}
}  // namespace

ServerScreen::ServerScreen() {
    auto* title = add<Label>("Choose a server", theme::type::largeTitle);
    title->setFrame({theme::layout::marginX, 250, 400, 44});
    auto* text = add<Label>("Pick a saved server, one found on your network, or add one by its address.",
                            theme::type::body, theme::color::textSecondary);
    text->setMaxLines(4);
    text->setFrame({theme::layout::marginX, 304, 380, 100});

    list_ = add<ScrollView>(Axis::Vertical);
    list_->setFrame({kListX, 0, theme::layout::screenWidth - kListX - theme::layout::marginX + 16, theme::layout::screenHeight});
    list_->setRevealInsets(70, 70);
    list_->setShowsIndicator(true);
    list_->content()->setSpacing(10);
    list_->content()->setPadding({16, 70, 16, 80});
    list_->content()->setRememberFocus(true);
    rebuild();
}

ServerScreen::~ServerScreen() {
    discoverToken_.cancel();
    connectToken_.cancel();
}

void ServerScreen::onShow() {
    visible_ = true;
    rebuild();
    discover();
}

void ServerScreen::onHide() { visible_ = false; }

void ServerScreen::discover() {
    if (discovering_) return;
    if (!platform::networkConnected()) {
        rediscoverIn_ = kRediscoverSeconds;
        return;
    }
    discovering_ = true;
    if (!searchedOnce_) rebuild();  // shows "Searching..."
    discoverToken_.cancel();
    discoverToken_.reset();
    jf::discoverServers(
        [this](jf::Result<std::vector<jf::DiscoveredServer>> r) {
            discovering_ = false;
            rediscoverIn_ = kRediscoverSeconds;
            // Background re-scans only rebuild the list when the result changed.
            bool changed = !searchedOnce_;
            searchedOnce_ = true;
            if (r) {
                std::vector<std::string> before, after;
                for (const auto& s : discovered_) before.push_back(s.id + s.address);
                for (const auto& s : *r) after.push_back(s.id + s.address);
                changed = changed || before != after;
                discovered_ = std::move(r.value);
            }
            if (changed) rebuild();
        },
        2500, discoverToken_);
}

void ServerScreen::rebuild() {
    Box* box = list_->content();
    size_t focusIdx = SIZE_MAX;
    for (size_t i = 0; i < box->childCount(); ++i)
        if (box->childAt(i)->hasFocusWithin()) focusIdx = i;
    box->clearChildren();

    const auto saved = jf::AccountStore::instance().servers();
    if (!saved.empty()) {
        box->addChild(heading("SAVED SERVERS"));
        for (const auto& s : saved) {
            std::string detail;
            for (const auto& u : s.users)
                if (u.hasToken()) {
                    detail = u.name;
                    break;
                }
            if (detail.empty() && !s.users.empty())
                detail = std::format("{} user{}", s.users.size(), s.users.size() == 1 ? "" : "s");
            auto* row = static_cast<ActionRow*>(box->addChild(std::make_unique<ActionRow>(
                gfx::Icon::Server, s.name.empty() ? s.url : s.name, s.url, detail)));
            row->setHints({{Key::Y, "Remove"}});
            const jf::StoredServer server = s;
            row->onActivate = [server] {
                nav().push(std::make_unique<LoginScreen>(jf::Client::fromStored(server)));
            };
            row->onSecondary = [this, server] {
                present(Dialog::confirm("Remove server?",
                                        "\"" + (server.name.empty() ? server.url : server.name) +
                                            "\" and its saved sign-ins will be forgotten. Downloads stay on the SD card.",
                                        "Remove", "Cancel", guard([this, id = server.id](bool ok) {
                                            if (!ok) return;
                                            jf::AccountStore::instance().removeServer(id);
                                            timeline().after(0.0f, [this] { rebuild(); }, lifetime());
                                        })));
            };
        }
    }

    box->addChild(heading("ON YOUR NETWORK"));
    size_t shown = 0;
    for (const auto& d : discovered_) {
        const bool known = std::any_of(saved.begin(), saved.end(), [&](const jf::StoredServer& s) { return s.id == d.id; });
        if (known) continue;
        ++shown;
        auto* row = static_cast<ActionRow*>(box->addChild(std::make_unique<ActionRow>(
            gfx::Icon::Wifi, d.name.empty() ? "Jellyfin server" : d.name, d.address, "Found nearby")));
        row->setIconColor(theme::color::success);
        const std::string address = d.address, name = d.name.empty() ? d.address : d.name;
        row->onActivate = [this, address, name] { connect(address, name); };
    }
    if (shown == 0) {
        const bool searching = discovering_ && !searchedOnce_;
        std::string msg = searching ? "Searching your network..."
                          : !platform::networkConnected() ? "Not connected to a network."
                          : discovered_.empty() ? "No servers found nearby. You can add one by address."
                                                : "All servers on this network are already saved.";
        auto* row = static_cast<ActionRow*>(box->addChild(std::make_unique<ActionRow>(
            searching ? gfx::Icon::Search : gfx::Icon::Wifi, msg, "", "")));
        row->setFocusable(false);
        row->setChevron(false);
        row->setBusy(searching);
        row->setIconColor(theme::color::textTertiary);
    }

    box->addChild(heading("MANUAL"));
    auto* add = static_cast<ActionRow*>(box->addChild(std::make_unique<ActionRow>(
        gfx::Icon::Plus, "Add server", "Enter an address like 192.168.1.10:8096 or jellyfin.example.com")));
    add->onActivate = [this] { addServer(); };

    const auto storage = downloads::DownloadManager::instance().storage();
    if (storage.completedCount > 0) {
        auto* off = static_cast<ActionRow*>(box->addChild(std::make_unique<ActionRow>(
            gfx::Icon::Download, "Offline downloads",
            std::format("{} title{} available without a connection", storage.completedCount,
                        storage.completedCount == 1 ? "" : "s"))));
        off->setIconColor(theme::color::warning);
        off->onActivate = [] { enterOffline(nullptr); };
    }

    // Restore focus by position, else focus the first actionable row.
    View* target = nullptr;
    if (focusIdx != SIZE_MAX)
        for (size_t i = std::min(focusIdx, box->childCount() - 1); i < box->childCount() && !target; ++i)
            if (box->childAt(i)->canFocus()) target = box->childAt(i);
    if (!target && (focusIdx != SIZE_MAX || !focus().focused() || !focus().focused()->isDescendantOf(this)))
        for (size_t i = 0; i < box->childCount() && !target; ++i)
            if (box->childAt(i)->focusable()) target = box->childAt(i);
    if (target) focus().setFocus(target);
}

void ServerScreen::addServer(const std::string& initial) {
    KeyboardOptions opts;
    opts.header = "Add server";
    opts.sub = "Address of your Jellyfin server";
    opts.guide = "192.168.1.10:8096";
    opts.initial = initial;
    opts.okLabel = "Connect";
    opts.kind = KeyboardKind::Url;
    requestText(std::move(opts), guard([this](std::string text) { connect(text, text); }));
}

void ServerScreen::connect(const std::string& address, const std::string& label) {
    connectToken_.cancel();
    connectToken_.reset();
    auto busy = std::make_unique<BusyModal>("Connecting to " + label + "...", [this] { connectToken_.cancel(); });
    busy_ = busy.get();
    present(std::move(busy));
    jf::Client::connect(address).then(
        [this, address](jf::Result<jf::ClientPtr> r) {
            if (BusyModal* b = busy_.get()) b->finish();
            if (!r) {
                DialogSpec spec;
                spec.title = "Can't connect";
                spec.message = errorMessage(r.error);
                spec.icon = gfx::Icon::Warning;
                spec.iconColor = theme::color::warning;
                spec.buttons = {{"Cancel", ButtonStyle::Secondary, {}},
                                {"Edit address", ButtonStyle::Primary, guard([this, address] { addServer(address); })}};
                spec.cancelIndex = 0;
                spec.defaultIndex = 1;
                present(std::make_unique<Dialog>(std::move(spec)));
                return;
            }
            rebuild();
            nav().push(std::make_unique<LoginScreen>(*r));
        },
        connectToken_);
}

void ServerScreen::update(float dt) {
    t_ += dt;
    if (visible_ && !discovering_) {
        rediscoverIn_ -= dt;
        if (rediscoverIn_ <= 0) discover();
    }
    Screen::update(dt);
}

void ServerScreen::draw(Renderer& r) {
    Screen::draw(r);
    using namespace theme;
    const Rect b = bounds();
    const Vec2 g{260 + std::sin(t_ * 0.3f) * 40.0f, 300 + std::cos(t_ * 0.25f) * 30.0f};
    r.drawShadow({g.x - 140, g.y - 140, 280, 280}, 140, 280, color::accentDeep.withAlpha(0.28f));
    r.fillRect({kListX - 1, 60, 1, b.h - 120}, color::separator);
    const gfx::TextStyle brand{40, gfx::FontWeight::Display, 1.0f, -1.0f};
    auto l = gfx::text::layout("karafin", brand);
    r.drawText(*l, {layout::marginX, 180}, color::textPrimary);
    r.fillCircle({layout::marginX + l->width + 8, 180 + l->firstBaseline - 6}, 6, color::accent);
}

}  // namespace karafin::screens
