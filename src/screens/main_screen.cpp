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
#include "ui/root.hpp"

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
}  // namespace

// Brand, scrim and account chip. The tab bar is a sibling drawn on top.
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

        // Wordmark with an accent dot.
        const gfx::TextStyle brand{28, gfx::FontWeight::Display, 1.0f, -0.8f};
        auto l = gfx::text::layout("karafin", brand);
        r.drawText(*l, {layout::marginX, 30}, color::textPrimary);
        r.fillCircle({layout::marginX + l->width + 6, 30 + l->firstBaseline - 4}, 4, color::accent);

        const float right = width() - layout::marginX;
        if (mode_ == MainMode::Offline) {
            auto t = gfx::text::layout("Offline mode", type::caption);
            const float w = t->width + 46;
            const Rect pill{right - w, 32, w, 30};
            r.fillRoundRect(pill, 15, color::warning.withAlpha(0.16f));
            gfx::drawIcon(r, gfx::Icon::Wifi, {pill.x + 12, pill.y + 7, 16, 16}, color::warning);
            r.drawText(*t, {pill.x + 34, pill.y + (30 - t->height) * 0.5f}, color::warning);
            return;
        }
        if (userName_.empty()) return;
        const float av = 34;
        const Rect a{right - av, 30, av, av};
        if (const gfx::Texture* tex = avatar_ ? avatar_->texture() : nullptr) {
            r.drawImage(*tex, a, {.fit = gfx::ImageFit::Cover, .radius = av * 0.5f});
        } else {
            r.fillCircle(a.center(), av * 0.5f, placeholderColor(userName_).lighter(0.12f));
            gfx::drawIcon(r, gfx::Icon::User, a.inset(8), Color::white(0.85f));
        }
        auto n = gfx::text::layout(userName_, type::callout, {.maxWidth = 160});
        r.drawText(*n, {a.x - 10 - n->width, a.y + (av - n->height) * 0.5f}, color::textSecondary);
    }

private:
    MainMode mode_;
    std::string userName_;
    gfx::ImagePtr avatar_;
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

    if (mode_ == MainMode::Offline) {
        addShortcut(Key::Minus, [] { restartSession(); });
        setHints({{Key::L, "Section"}, {Key::Minus, "Reconnect"}});
    } else {
        setHints({{Key::L, "Section"}});
    }
    current_ = initial;
    showSection(initial);
}

size_t MainScreen::tabIndex(Section s) const {
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i] == s) return i;
    return 0;
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
