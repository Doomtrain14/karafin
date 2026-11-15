#include "screens/widgets/hero.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "screens/common.hpp"
#include "screens/item_meta.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kContentWidth = 600;
constexpr float kLogoMaxW = 420;
constexpr float kLogoMaxH = 120;
constexpr float kInterval = 10.0f;
constexpr float kDotsBottom = 34;
constexpr float kTopLimit = 104;  // keep clear of the main screen header
}  // namespace

HeroCarousel::HeroCarousel() {
    setRememberFocus(true);
    setCullMargin(0);

    content_ = add<Box>(Axis::Vertical);
    content_->setSpacing(12).setAlign(Align::Start);
    overline_ = content_->add<Label>("", theme::type::overline, theme::color::accentBright);
    logo_ = content_->add<ImageView>();
    logo_->setFit(gfx::ImageFit::Contain);
    logo_->setShimmer(false);
    logo_->setPlaceholderColor(Color::transparent());
    logo_->setVisible(false);
    title_ = content_->add<Label>("", theme::type::hero);
    title_->setMaxLines(2);
    title_->setPreferredSize(kContentWidth, -1);
    chips_ = content_->add<ChipRow>();
    overview_ = content_->add<Label>("", theme::type::body, theme::color::textSecondary);
    overview_->setMaxLines(3);
    overview_->setPreferredSize(kContentWidth - 40, -1);
    buttons_ = content_->add<Box>(Axis::Horizontal);
    buttons_->setSpacing(14).setMargin({0, 10, 0, 0});
    buttons_->setRememberFocus(true);
    play_ = buttons_->add<ActionButton>("Play", gfx::Icon::Play, ButtonStyle::Primary);
    info_ = buttons_->add<ActionButton>("More info", gfx::Icon::Info, ButtonStyle::Secondary);
    play_->onActivate = [this] {
        if (auto* e = current(); e && onPlay) onPlay(e->item);
    };
    info_->onActivate = [this] {
        if (auto* e = current(); e && onInfo) onInfo(e->item);
    };
    buttons_->setRememberedFocus(play_);
    setRememberedFocus(play_);

    skeleton_ = add<Skeleton>();
    skeleton_->addRect({0, 0, 120, 12}, 5)
        .addRect({0, 26, 420, 44}, 10)
        .addRect({0, 86, 300, 18}, 6)
        .addRect({0, 120, 560, 14}, 6)
        .addRect({0, 144, 520, 14}, 6)
        .addRect({0, 168, 380, 14}, 6)
        .addRect({0, 206, 130, 46}, theme::radius::md)
        .addRect({144, 206, 150, 46}, theme::radius::md);
    setLoading();
}

void HeroCarousel::setLoading() {
    entries_.clear();
    content_->setVisible(false);
    skeleton_->setVisible(true);
}

void HeroCarousel::setEntries(std::vector<HeroEntry> entries) {
    entries_ = std::move(entries);
    skeleton_->setVisible(false);
    content_->setVisible(!entries_.empty());
    index_ = 0;
    timer_ = 0;
    if (entries_.empty()) return;
    bind();
    content_->alpha.set(0.0f);
    content_->alpha.tweenTo(1.0f, 0.45f, ease::outCubic);
    content_->offset.set({0, 18});
    content_->offset.springTo({0, 0}, theme::motion::gentle);
    if (onCurrentChanged) onCurrentChanged(entries_[0]);
}

void HeroCarousel::advance(int dir) {
    if (entries_.size() < 2) return;
    const size_t n = entries_.size();
    const size_t next = dir > 0 ? (index_ + 1) % n : (index_ + n - 1) % n;
    show(next, dir > 0 ? 1 : -1);
}

void HeroCarousel::show(size_t index, int dir) {
    if (index >= entries_.size()) return;
    index_ = index;
    timer_ = 0;
    if (anim_) timeline().cancel(anim_);
    const float d = static_cast<float>(dir);
    const float startAlpha = content_->alpha.value();
    const Vec2 startOff = content_->offset.value();
    anim_ = timeline().animate(
        0.16f, ease::inQuad,
        [this, d, startAlpha, startOff](float t) {
            content_->alpha.set(startAlpha * (1.0f - t));
            content_->offset.set({gfx::lerp(startOff.x, -d * 22.0f, t), startOff.y});
        },
        [this, d] {
            anim_ = 0;
            bind();
            content_->offset.set({d * 28.0f, 0});
            content_->offset.springTo({0, 0}, theme::motion::standard);
            content_->alpha.tweenTo(1.0f, 0.32f, ease::outCubic);
        },
        lifetime());
    if (onCurrentChanged) onCurrentChanged(entries_[index_]);
}

void HeroCarousel::bind() {
    const HeroEntry* e = current();
    if (!e) return;
    const jellyfin::BaseItem& it = e->item;
    overline_->setText(e->tag);
    overline_->setVisible(!e->tag.empty());
    std::string title = it.isEpisode() && !it.seriesName.empty() ? it.seriesName : it.name;
    title_->setText(title);
    title_->setVisible(true);
    logo_->setVisible(false);
    logoShown_ = false;
    logoImage_.reset();
    if (const std::string url = logoUrl(it); !url.empty()) {
        gfx::ImageRequest req = imageRequest(url, kLogoMaxW, kLogoMaxH);
        req.fit = gfx::ImageFit::Contain;
        logoImage_ = gfx::ImageCache::instance().load(req);
        checkLogo();
    }
    std::vector<Chip> chips;
    if (it.isEpisode()) {
        chips.push_back({it.episodeTag(), ChipStyle::Filled});
        chips.push_back({it.name, ChipStyle::Text});
    } else {
        chips = itemChips(it);
        if (auto g = genresText(it, 2); !g.empty()) chips.push_back({g, ChipStyle::Text});
    }
    chips_->setChips(std::move(chips));
    overview_->setText(it.overview);
    overview_->setVisible(!it.overview.empty());
    const bool resume = it.userData.playbackPositionTicks > 0 && !it.userData.played && it.isPlayable();
    play_->setLabel(resume ? "Resume" : "Play");
    play_->setTooltip(resume ? remainingText(it) : std::string());
    setNeedsLayout();
}

void HeroCarousel::checkLogo() {
    if (logoShown_ || !logoImage_) return;
    if (logoImage_->failed()) {
        logoImage_.reset();
        return;
    }
    if (!logoImage_->ready()) return;
    const float aspect = logoImage_->aspect();
    if (aspect <= 0.05f) return;
    float h = std::min(kLogoMaxH, kLogoMaxW / aspect);
    float w = h * aspect;
    if (w > kLogoMaxW) {
        w = kLogoMaxW;
        h = w / aspect;
    }
    logo_->setImage(logoImage_);
    logo_->setPreferredSize(std::round(w), std::round(h));
    logo_->setVisible(true);
    title_->setVisible(false);
    logoShown_ = true;
    setNeedsLayout();
}

Vec2 HeroCarousel::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    return {pref.x >= 0 ? pref.x : available.x, pref.y >= 0 ? pref.y : kHeight};
}

void HeroCarousel::layout() {
    const float x = theme::layout::marginX;
    const Vec2 m = content_->measure({kContentWidth, std::numeric_limits<float>::infinity()});
    // Room under the buttons for the focused button's caption ("1h 20m left") above the dots.
    const float bottom = height() - kDotsBottom - 44;
    content_->setFrame({x, std::max(kTopLimit, bottom - m.y), kContentWidth, m.y});
    const Vec2 s = skeleton_->measure({kContentWidth, 400});
    skeleton_->setFrame({x, bottom - s.y, s.x, s.y});
}

View* HeroCarousel::navigateFocus(View* from, NavDir dir) {
    if (entries_.size() < 2) return nullptr;
    if (dir == NavDir::Right && from == info_) {
        advance(1);
        return View::focusBlocked();
    }
    if (dir == NavDir::Left && from == play_) {
        advance(-1);
        return View::focusBlocked();
    }
    return nullptr;
}

bool HeroCarousel::onInput(const InputEvent& e) {
    timer_ = 0;
    return View::onInput(e);
}

void HeroCarousel::update(float dt) {
    checkLogo();
    if (auto_ && entries_.size() > 1 && content_->visible()) {
        timer_ += dt;
        if (timer_ >= kInterval) advance(1);
    }
    View::update(dt);
}

void HeroCarousel::draw(Renderer& r) {
    if (entries_.size() < 2 || !content_->visible()) return;
    // Page dots; the current one is a pill that fills up until the next auto-advance.
    float x = theme::layout::marginX;
    const float y = height() - kDotsBottom;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (i == index_) {
            const Rect pill{x, y - 3, 28, 6};
            r.fillRoundRect(pill, 3, Color::white(0.25f));
            const float p = auto_ ? std::clamp(timer_ / kInterval, 0.0f, 1.0f) : 1.0f;
            r.fillRoundRect({pill.x, pill.y, std::max(6.0f, pill.w * p), pill.h}, 3, Color::white(0.9f));
            x += 28 + 8;
        } else {
            r.fillCircle({x + 3, y}, 3, Color::white(0.3f));
            x += 6 + 8;
        }
    }
}

}  // namespace karafin::screens
