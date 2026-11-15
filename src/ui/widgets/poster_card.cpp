#include "ui/widgets/poster_card.hpp"

#include <algorithm>

namespace karafin::ui {
namespace {
constexpr float kTitleGap = 10;
}

Vec2 PosterCard::artSize(CardShape shape) {
    switch (shape) {
        case CardShape::Wide: return {theme::card::wideWidth, theme::card::wideHeight};
        case CardShape::Square: return {theme::card::squareSize, theme::card::squareSize};
        case CardShape::Poster: break;
    }
    return {theme::card::posterWidth, theme::card::posterHeight};
}

PosterCard::PosterCard(CardShape shape, bool showTitles) : shape_(shape), showTitles_(showTitles) {
    setFocusable(true);
    setCullMargin(60);
    image_ = add<ImageView>();
    image_->setRadius(theme::card::radius);
    if (showTitles_) {
        title_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
        subtitle_ = add<Label>("", theme::type::footnote, theme::color::textTertiary);
    }
    const Vec2 art = artSize(shape_);
    // Scale around the artwork's center so titles drift down slightly as it grows.
    setPivot({0.5f, (art.y * 0.5f) / std::max(1.0f, measure({art.x, 10000}).y)});
}

Rect PosterCard::artRect() const {
    const Vec2 art = artSize(shape_);
    return {0, 0, width() > 0 ? width() : art.x, art.y * ((width() > 0 ? width() : art.x) / art.x)};
}

void PosterCard::setImageUrl(std::string url, http::Headers headers) {
    image_->setUrl(std::move(url), std::move(headers));
}

void PosterCard::setImageRequest(gfx::ImageRequest request) { image_->setRequest(std::move(request)); }

void PosterCard::setImage(gfx::ImagePtr image) { image_->setImage(std::move(image)); }

void PosterCard::setTitle(std::string title) {
    titleText_ = title;
    if (title_) title_->setText(std::move(title));
}

void PosterCard::setSubtitle(std::string subtitle) {
    if (subtitle_) subtitle_->setText(std::move(subtitle));
}

void PosterCard::setProgress(float progress) {
    if (progress < 0) progress_.set(-1.0f);
    else if (progress_.value() < 0) progress_.set(std::clamp(progress, 0.0f, 1.0f));
    else progress_.springTo(std::clamp(progress, 0.0f, 1.0f), theme::motion::standard);
}

void PosterCard::setPlaceholder(Color color, gfx::Icon icon) {
    placeholder_ = color;
    placeholderIcon_ = icon;
    image_->setPlaceholderColor(color);
}

Vec2 PosterCard::measure(Vec2 available) {
    (void)available;
    const Vec2 pref = preferredSize();
    const Vec2 art = artSize(shape_);
    const float w = pref.x >= 0 ? pref.x : art.x;
    // The art keeps its aspect at a custom width (artRect()), so the labels move with it.
    float h = art.y * (w / art.x);
    if (showTitles_) {
        h += kTitleGap + gfx::text::lineHeight(theme::type::callout);
        h += gfx::text::lineHeight(theme::type::footnote);
    }
    return {w, pref.y >= 0 ? pref.y : h};
}

void PosterCard::layout() {
    const Rect art = artRect();
    image_->setFrame(art);
    if (showTitles_) {
        const float lh1 = gfx::text::lineHeight(theme::type::callout);
        title_->setFrame({2, art.bottom() + kTitleGap, art.w - 4, lh1});
        subtitle_->setFrame({2, art.bottom() + kTitleGap + lh1, art.w - 4, gfx::text::lineHeight(theme::type::footnote)});
        setPivot({0.5f, art.h * 0.5f / std::max(1.0f, height())});
    }
}

void PosterCard::onFocus() {
    focus_.springTo(1.0f, theme::motion::snappy);
    scale.springTo(theme::card::focusScale, theme::motion::focus);
    setZ(10);
    if (title_) title_->color.springTo(theme::color::textPrimary, theme::motion::snappy);
    if (subtitle_) subtitle_->color.springTo(theme::color::textSecondary, theme::motion::snappy);
}

void PosterCard::onBlur() {
    focus_.springTo(0.0f, theme::motion::standard);
    scale.springTo(1.0f, theme::motion::focus);
    if (title_) title_->color.springTo(theme::color::textSecondary, theme::motion::standard);
    if (subtitle_) subtitle_->color.springTo(theme::color::textTertiary, theme::motion::standard);
}

bool PosterCard::activate() {
    if (!enabled()) return false;
    scale.set(theme::card::focusScale * 0.95f);
    scale.springTo(focused() ? theme::card::focusScale : 1.0f, theme::motion::bouncy);
    return View::activate();
}

void PosterCard::onTouchDown() { scale.springTo(theme::card::pressScale, theme::motion::snappy); }

void PosterCard::onTouchCancel() {
    scale.springTo(focused() ? theme::card::focusScale : 1.0f, theme::motion::focus);
}

void PosterCard::update(float dt) {
    focus_.update(dt);
    progress_.update(dt);
    // Drop back to the base layer once the shrink animation has finished.
    if (!focused() && z() != 0 && scale.value() < 1.002f) setZ(0);
    View::update(dt);
}

void PosterCard::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const Rect art = artRect();
    const float rad = card::radius;
    // Resting shadow -> lifted shadow, plus an accent-tinted glow when focused.
    const float blur = gfx::lerp(shadowRest.blur, shadowFocus.blur, f);
    const float oy = gfx::lerp(shadowRest.offsetY, shadowFocus.offsetY, f);
    const float a = gfx::lerp(shadowRest.alpha, shadowFocus.alpha, f);
    r.drawShadow(art, rad, blur, Color::black(a), {0, oy});
    if (f > 0.01f) {
        const Color glow = image_->loaded() && image_->image()->hasDominantColor()
                               ? image_->image()->dominantColor().lighter(0.2f)
                               : color::accent;
        r.drawShadow(art, rad, 46, glow.withAlpha(0.38f * f), {0, 8}, 2);
    }
}

void PosterCard::drawOverlay(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const Rect art = artRect();
    const float rad = card::radius;

    // Placeholder title when there is no artwork.
    const bool hasArt = image_->loaded();
    if (!hasArt && !(image_->image() && image_->image()->loading()) && !titleText_.empty()) {
        gfx::TextOptions opt;
        opt.maxWidth = art.w - 24;
        opt.maxLines = 3;
        opt.align = gfx::TextAlign::Center;
        auto layout = gfx::text::layout(titleText_, type::headline, opt);
        const float iconS = 28;
        const float total = iconS + 10 + layout->height;
        float y = (art.h - total) * 0.5f;
        gfx::drawIcon(r, placeholderIcon_, {(art.w - iconS) * 0.5f, y, iconS, iconS}, Color::white(0.55f));
        y += iconS + 10;
        r.drawText(*layout, {(art.w - layout->width) * 0.5f, y}, Color::white(0.85f));
    }

    // Bottom scrim + progress bar.
    const float p = progress_.value();
    if (p >= 0) {
        r.fillRoundRect({art.x, art.bottom() - art.h * 0.32f, art.w, art.h * 0.32f}, rad,
                        gfx::Gradient::vertical(Color::black(0.0f), Color::black(0.6f)));
        const Rect track{art.x + 10, art.bottom() - 13, art.w - 20, 5};
        r.fillRoundRect(track, 2.5f, color::progressTrack);
        if (p > 0) r.fillRoundRect({track.x, track.y, std::max(5.0f, track.w * p), track.h}, 2.5f, accentGradient);
    }

    // Badges (top-right).
    if (watched_) {
        const Vec2 c{art.right() - 18, art.y + 18};
        r.drawShadow({c.x - 12, c.y - 12, 24, 24}, 12, 8, Color::black(0.4f), {0, 2});
        r.fillCircle(c, 12, color::accent);
        gfx::drawIcon(r, gfx::Icon::Check, {c.x - 8, c.y - 8, 16, 16}, Color::white(), 1.3f);
    } else if (!badge_.empty()) {
        auto layout = gfx::text::layout(badge_, type::footnote, {.maxWidth = std::max(10.0f, art.w - 30)});
        const float w = std::max(24.0f, layout->width + 14);
        const Rect pill{art.right() - 8 - w, art.y + 8, w, 22};
        r.drawShadow(pill, 11, 8, Color::black(0.4f), {0, 2});
        r.fillRoundRect(pill, 11, color::accent);
        r.drawText(*layout, {pill.x + (w - layout->width) * 0.5f, pill.y + (22 - layout->height) * 0.5f},
                   Color::white());
    }

    if (f > 0.01f) {
        // Specular shine from the top-left, then the focus ring just outside the art.
        r.fillRoundRect(art, rad,
                        gfx::Gradient{Color::white(0.16f * f), Color::white(0.04f * f), Color::white(0.0f),
                                      Color::white(0.03f * f)});
        r.strokeRoundRect(art.expanded(3.5f), rad + 3.5f, 2.5f, color::focusRing.mulAlpha(f));
    }
}

}  // namespace karafin::ui
