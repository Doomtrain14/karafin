#include "ui/widgets/tab_bar.hpp"

#include <algorithm>
#include <cmath>

#include "gfx/text.hpp"
#include "ui/root.hpp"
#include "ui/screen.hpp"
#include "ui/theme.hpp"

namespace karafin::ui {
namespace {
constexpr float kBarHeight = 44;
constexpr float kTabPadX = 20;
constexpr float kInset = 4;
}  // namespace

class TabBar::Tab : public View {
public:
    Tab(TabBar* bar, size_t index) : bar_(bar), index_(index) { setFocusable(true); }
    bool activate() override {
        bar_->select(index_);
        return true;
    }
    void draw(Renderer& r) override {
        using namespace theme;
        const bool sel = bar_->selected_ == index_;
        const float f = std::clamp(bar_->focusAmt_.value(), 0.0f, 1.0f);
        Color ink = color::textSecondary;
        if (sel) ink = Color::lerp(color::textPrimary, color::textOnLight, f);
        else if (focused()) ink = color::textPrimary;
        auto layout = gfx::text::layout(bar_->titles_[index_], type::headline);
        const float capH = gfx::text::capHeight(type::headline);
        const float y = (height() - capH) * 0.5f - (layout->firstBaseline - capH);
        r.drawText(*layout, {(width() - layout->width) * 0.5f, y}, ink);
    }

private:
    TabBar* bar_;
    size_t index_;
};

TabBar::TabBar(std::vector<std::string> titles) {
    setRememberFocus(true);
    setTitles(std::move(titles));
}

void TabBar::setTitles(std::vector<std::string> titles) {
    clearChildren();
    tabs_.clear();
    titles_ = std::move(titles);
    for (size_t i = 0; i < titles_.size(); ++i) tabs_.push_back(add<Tab>(this, i));
    selected_ = std::min(selected_, titles_.empty() ? size_t{0} : titles_.size() - 1);
    placed_ = false;
    setNeedsLayout();
}

void TabBar::select(size_t index, bool notify) {
    if (index >= titles_.size()) return;
    const bool changed = index != selected_;
    selected_ = index;
    setRememberedFocus(tabs_[index]);
    if (hasFocusWithin() && Root::exists()) Root::instance().focus().setFocus(tabs_[index]);
    if (changed && notify && onSelect) onSelect(index);
}

void TabBar::bindShoulderButtons(Screen* screen) {
    if (!screen) return;
    screen->addShortcut(Key::L, guard([this] { selectPrevious(); }));
    screen->addShortcut(Key::R, guard([this] { selectNext(); }));
}

Vec2 TabBar::measure(Vec2 available) {
    (void)available;
    float w = kInset * 2;
    for (const auto& t : titles_) w += gfx::text::measure(t, theme::type::headline).x + kTabPadX * 2;
    return {std::ceil(w), kBarHeight};
}

void TabBar::layout() {
    // Content is centered inside the frame.
    const float total = measure({width(), height()}).x;
    float x = std::max(0.0f, (width() - total) * 0.5f) + kInset;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const float w = gfx::text::measure(titles_[i], theme::type::headline).x + kTabPadX * 2;
        tabs_[i]->setFrame({x, kInset, w, kBarHeight - kInset * 2});
        x += w;
    }
}

void TabBar::onFocusWithin(View* descendant) {
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i] == descendant && i != selected_) {
            selected_ = i;
            if (onSelect) onSelect(i);
        }
}

View* TabBar::defaultFocus() {
    if (tabs_.empty() || !visible()) return nullptr;
    return tabs_[std::min(selected_, tabs_.size() - 1)];
}

View* TabBar::navigateFocus(View* from, NavDir dir) {
    if (dir != NavDir::Left && dir != NavDir::Right) return nullptr;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i] != from) continue;
        if (dir == NavDir::Left) return i > 0 ? tabs_[i - 1] : View::focusBlocked();
        return i + 1 < tabs_.size() ? tabs_[i + 1] : View::focusBlocked();
    }
    return nullptr;
}

void TabBar::update(float dt) {
    if (!tabs_.empty()) {
        const Rect f = tabs_[std::min(selected_, tabs_.size() - 1)]->frame();
        if (!placed_ && f.w > 0) {
            pillX_.set(f.x);
            pillW_.set(f.w);
            placed_ = true;
        } else {
            pillX_.springTo(f.x, theme::motion::snappy);
            pillW_.springTo(f.w, theme::motion::snappy);
        }
    }
    focusAmt_.springTo(hasFocusWithin() ? 1.0f : 0.0f, theme::motion::snappy);
    pillX_.update(dt);
    pillW_.update(dt);
    focusAmt_.update(dt);
    View::update(dt);
}

void TabBar::draw(Renderer& r) {
    using namespace theme;
    if (tabs_.empty()) return;
    const float total = measure({width(), height()}).x;
    const Rect capsule{std::max(0.0f, (width() - total) * 0.5f), 0, total, kBarHeight};
    r.fillRoundRect(capsule, kBarHeight * 0.5f, Color(1, 1, 1, 0.07f));
    const float f = std::clamp(focusAmt_.value(), 0.0f, 1.0f);
    const Rect pill{pillX_.value(), kInset, pillW_.value(), kBarHeight - kInset * 2};
    if (f > 0.01f) r.drawShadow(pill, pill.h * 0.5f, 16, Color::black(0.4f * f), {0, 4});
    r.fillRoundRect(pill, pill.h * 0.5f, Color::lerp(Color(1, 1, 1, 0.16f), Color::white(0.95f), f));
}

}  // namespace karafin::ui
