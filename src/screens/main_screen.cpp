#include "screens/main_screen.hpp"

#include <algorithm>

#include "gfx/text.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/downloads_section.hpp"
#include "screens/home_section.hpp"
#include "screens/libraries_section.hpp"
#include "screens/search_section.hpp"
#include "screens/session.hpp"
#include "screens/settings_section.hpp"
#include "screens/wordmark.hpp"
#include "ui/actions.hpp"
#include "ui/battery_indicator.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
const char* sectionTitle(Section s) {
    switch (s) {
        case Section::Home: return "Home";
        case Section::Libraries: return "Libraries";
        case Section::Search: return "Search";
        case Section::Downloads: return "Downloads";
        case Section::Settings: return "Settings";
        case Section::Count: break;
    }
    return "";
}
size_t idx(Section s) { return static_cast<size_t>(s); }

gfx::Icon sectionIcon(Section s) {
    switch (s) {
        case Section::Home: return gfx::Icon::Home;
        case Section::Libraries: return gfx::Icon::Library;
        case Section::Search: return gfx::Icon::Search;
        case Section::Downloads: return gfx::Icon::Download;
        case Section::Settings: return gfx::Icon::Settings;
        case Section::Count: break;
    }
    return gfx::Icon::None;
}

void showControlsGuide() {
    present(Dialog::alert("Controls",
                          "A  Play / resume a title or episode, or select\n"
                          "X  Actions for the selected item and this page\n"
                          "+  App menu: sections, reconnect and quit\n"
                          "B  Back / close a menu\n"
                          "L / R  Switch tabs or seasons\n"
                          "ZL / ZR  Page through long lists\n\n"
                          "In the player, the on-screen buttons show the controls."));
}
}  // namespace

MainScreen* mainScreen() {
    for (size_t i = 0; i < nav().depth(); ++i)
        if (auto* m = dynamic_cast<MainScreen*>(nav().at(i))) return m;
    return nullptr;
}

void showAppMenu() {
    MainScreen* main = mainScreen();
    if (!main) return;
    const bool onMain = nav().top() == main;
    ActionMenu menu;
    menu.setTitle("Karafin", onMain ? std::string("Menu") : std::string("Menu - leaves this page"));
    auto mark = std::make_shared<Wordmark>(26);
    menu.setTitleArt(mark->height(), [mark](Renderer& r, const Rect& box) { mark->draw(r, {box.x, box.y}); });
    auto addSection = [&](Section s) {
        Action a;
        a.title = sectionTitle(s);
        a.icon = sectionIcon(s);
        a.checked = onMain && main->current() == s;
        a.run = [s] {
            if (MainScreen* m = mainScreen()) m->navigateTo(s);
        };
        menu.add(std::move(a));
    };
    menu.section("Navigate");
    for (Section s : main->tabs())
        if (s != Section::Downloads && s != Section::Settings) addSection(s);
    menu.section("Manage");
    addSection(Section::Downloads);
    addSection(Section::Settings);
    menu.section("Application");
    if (main->mode() == MainMode::Offline)
        menu.add("Reconnect to server", gfx::Icon::Refresh, [] { restartSession(); });
    menu.add("Controls", gfx::Icon::Info, [] { showControlsGuide(); });
    menu.add("Quit Karafin", gfx::Icon::Exit, [] { root().confirmQuit(); });
    presentActions(std::move(menu), Key::Plus);
}

// Wordmark, scrim and account chip. The tab bar is a sibling drawn on top.
class MainScreen::Header : public View {
public:
    explicit Header(MainMode mode) : mode_(mode) {
        if (mode_ == MainMode::Online) {
            if (auto c = jellyfin::currentClient()) {
                const auto user = c->user();
                userName_ = user.name;
                const std::string url = c->userImageUrl(user.id, user.primaryImageTag, {.maxWidth = 96});
                if (!url.empty()) avatar_ = gfx::ImageCache::instance().load(imageRequest(url, 34, 34));
            }
        }
    }

    float scrim = 1.0f;

    bool hitTest(Vec2) const override { return false; }

    void draw(Renderer& r) override {
        using namespace theme;
        const Color bg = color::background;
        const float s = std::clamp(scrim, 0.0f, 1.0f);
        r.fillRect({0, 0, width(), 140}, gfx::Gradient::vertical(bg.withAlpha(0.55f + 0.4f * s), bg.withAlpha(0.0f)));

        wordmark_.draw(r, {layout::marginX, 47 - wordmark_.height() * 0.5f});

        const float right = width() - layout::marginX;
        float rightEdge = right;
        if (mode_ == MainMode::Offline) {
            auto t = gfx::text::layout("Offline mode", type::caption);
            const float w = t->width + 46;
            const Rect pill{right - w, 32, w, 30};
            r.fillRoundRect(pill, 15, color::warning.withAlpha(0.16f));
            gfx::drawIcon(r, gfx::Icon::Wifi, {pill.x + 12, pill.y + 7, 16, 16}, color::warning);
            r.drawText(*t, {pill.x + 34, pill.y + (30 - t->height) * 0.5f}, color::warning);
            battery_.draw(r, pill.x - 16, pill.center().y, color::textSecondary);
            return;
        }
        if (userName_.empty()) {
            battery_.draw(r, rightEdge, 47, color::textSecondary);
            return;
        }
        const float av = 34;
        const Rect a{right - av, 30, av, av};
        if (const gfx::Texture* tex = avatar_ ? avatar_->texture() : nullptr) {
            r.drawImage(*tex, a, {.fit = gfx::ImageFit::Cover, .radius = av * 0.5f});
        } else {
            r.fillCircle(a.center(), av * 0.5f, placeholderColor(userName_).lighter(0.12f));
            gfx::drawIcon(r, gfx::Icon::User, a.inset(8), Color::white(0.85f));
        }
        // Narrow enough that the battery indicator never reaches the centred tab bar.
        auto n = gfx::text::layout(userName_, type::callout, {.maxWidth = 120});
        const float nameX = a.x - 10 - n->width;
        r.drawText(*n, {nameX, a.y + (av - n->height) * 0.5f}, color::textSecondary);
        rightEdge = nameX - 20;
        battery_.draw(r, rightEdge, a.center().y, color::textSecondary);
    }

private:
    MainMode mode_;
    Wordmark wordmark_{24};
    std::string userName_;
    gfx::ImagePtr avatar_;
    BatteryIndicator battery_;
};

MainScreen::MainScreen(MainMode mode, Section initial) : mode_(mode) {
    if (mode_ == MainMode::Online)
        tabs_ = {Section::Home, Section::Libraries, Section::Search, Section::Downloads, Section::Settings};
    else
        tabs_ = {Section::Downloads, Section::Settings};
    if (std::find(tabs_.begin(), tabs_.end(), initial) == tabs_.end()) initial = tabs_.front();

    body_ = add<View>();
    body_->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    body_->setRememberFocus(true);
    body_->setHints({{Key::B, "Back"}});
    header_ = add<Header>(mode_);
    header_->setFrame({0, 0, theme::layout::screenWidth, kHeaderHeight});

    std::vector<std::string> titles;
    for (Section s : tabs_) titles.emplace_back(sectionTitle(s));
    tabBar_ = add<TabBar>(std::move(titles));
    tabBar_->setFrame({0, 26, theme::layout::screenWidth, 44});
    tabBar_->bindShoulderButtons(this);
    tabBar_->onSelect = [this](size_t i) {
        if (i < tabs_.size()) showSection(tabs_[i]);
    };

    setHints({{Key::L, "Previous tab"}, {Key::R, "Next tab"}});
    onAppMenu = [] { showAppMenu(); };
    current_ = initial;
    showSection(initial);
}

size_t MainScreen::tabIndex(Section s) const {
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i] == s) return i;
    return 0;
}

void MainScreen::navigateTo(Section s) {
    if (nav().top() != this) nav().popTo(this);
    showSection(s);
    SectionView* v = sections_[idx(s)];
    View* f = v ? v->defaultFocus() : nullptr;
    focus().setFocus(f ? f : tabBar_->defaultFocus());
}

SectionView* MainScreen::ensure(Section s) {
    if (SectionView* v = sections_[idx(s)]) return v;
    std::unique_ptr<SectionView> view;
    switch (s) {
        case Section::Home: view = std::make_unique<HomeSection>(); break;
        case Section::Libraries: view = std::make_unique<LibrariesSection>(); break;
        case Section::Search: view = std::make_unique<SearchSection>(); break;
        case Section::Downloads: view = std::make_unique<DownloadsSection>(mode_); break;
        case Section::Settings: view = std::make_unique<SettingsSection>(mode_); break;
        case Section::Count: return nullptr;
    }
    view->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    view->setVisible(false);
    auto* raw = static_cast<SectionView*>(body_->addChild(std::move(view)));
    sections_[idx(s)] = raw;
    return raw;
}

void MainScreen::showSection(Section s) {
    SectionView* next = ensure(s);
    if (!next) return;
    SectionView* old = sections_[idx(current_)];
    const bool same = old == next && next->visible();
    if (same) return;
    const bool focusInOld = old && old != next && old->hasFocusWithin();
    if (old && old != next && old->visible()) {
        old->onSectionHidden();
        old->setVisible(false);
    }
    current_ = s;
    next->setVisible(true);
    next->alpha.set(0.0f);
    next->alpha.tweenTo(1.0f, 0.34f, ease::outCubic);
    next->offset.set({0, 16});
    next->offset.springTo({0, 0}, theme::motion::standard);
    tabBar_->select(tabIndex(s), false);
    tabBar_->setHints({{Key::B, s == tabs_.front() ? std::string("Quit") : sectionTitle(tabs_.front())}});
    if (visibleNow_) {
        const bool first = !shownOnce_[idx(s)];
        shownOnce_[idx(s)] = true;
        next->onSectionShown(first);
    }
    if (focusInOld) {
        View* f = next->defaultFocus();
        focus().setFocus(f ? f : tabBar_->defaultFocus());
    }
}

void MainScreen::onShow() {
    visibleNow_ = true;
    if (SectionView* v = sections_[idx(current_)]) {
        const bool first = !shownOnce_[idx(current_)];
        shownOnce_[idx(current_)] = true;
        v->onSectionShown(first);
    }
}

void MainScreen::onHide() {
    visibleNow_ = false;
    if (SectionView* v = sections_[idx(current_)]) v->onSectionHidden();
}

bool MainScreen::onBack() {
    SectionView* cur = sections_[idx(current_)];
    if (cur && cur->hasFocusWithin()) {
        if (cur->onSectionBack()) return true;
        focus().setFocus(tabBar_->defaultFocus());
        return true;
    }
    if (current_ != tabs_.front()) {
        tabBar_->select(0);
        return true;
    }
    root().confirmQuit();
    return true;
}

void MainScreen::update(float dt) {
    if (SectionView* v = sections_[idx(current_)]) header_->scrim = v->headerScrim();
    Screen::update(dt);
}

}  // namespace karafin::screens
