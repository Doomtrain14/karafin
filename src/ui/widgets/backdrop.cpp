#include "ui/widgets/backdrop.hpp"

#include <algorithm>

namespace karafin::ui {

Backdrop::Backdrop() { setCullMargin(0); }

void Backdrop::setRequest(const gfx::ImageRequest& request) {
    setImage(request.url.empty() ? nullptr : gfx::ImageCache::instance().load(request));
}

void Backdrop::setImage(gfx::ImagePtr image) {
    if (image == current_.image) return;
    // The outgoing layer keeps whatever opacity it had, so rapid changes never pop.
    if (current_.image && current_.alpha.value() > 0.02f) {
        previous_.image = std::move(current_.image);
        previous_.alpha.set(current_.alpha.value());
        previous_.alpha.tweenTo(0.0f, 0.6f, ease::inOutSine);
    }
    current_.image = std::move(image);
    current_.alpha.set(0.0f);
}

void Backdrop::clear() { setImage(nullptr); }

void Backdrop::update(float dt) {
    if (current_.image && current_.image->ready() && current_.alpha.target() < 1.0f)
        current_.alpha.tweenTo(1.0f, 0.7f, ease::inOutSine);
    if (current_.image && current_.image->ready() && current_.image->hasDominantColor()) {
        const Color d = current_.image->dominantColor();
        tint_.springTo(Color::lerp(theme::color::background, d.darker(0.55f), 0.55f), theme::motion::gentle);
    }
    current_.alpha.update(dt);
    previous_.alpha.update(dt);
    tint_.update(dt);
    if (previous_.image && previous_.alpha.value() <= 0.001f && !previous_.alpha.animating()) previous_.image.reset();
    View::update(dt);
}

void Backdrop::draw(Renderer& r) {
    const Rect b = bounds();
    if (ambient_) r.fillRect(b, tint_.value());
    auto drawLayer = [&](Layer& l) {
        if (!l.image) return;
        const float a = std::clamp(l.alpha.value(), 0.0f, 1.0f) * intensity_;
        if (a <= 0.003f) return;
        if (const gfx::Texture* t = l.image->texture()) r.drawImage(*t, b, {.fit = gfx::ImageFit::Cover, .tint = Color::white(a), .focus = {0.5f, 0.3f}});
    };
    drawLayer(previous_);
    drawLayer(current_);
    const Color bg = theme::color::background;
    if (leftScrim_)
        r.fillRect({0, 0, b.w * 0.62f, b.h}, gfx::Gradient::horizontal(bg.withAlpha(0.92f), bg.withAlpha(0.0f)));
    if (bottomScrim_)
        r.fillRect({0, b.h * 0.35f, b.w, b.h * 0.65f + 1}, gfx::Gradient::vertical(bg.withAlpha(0.0f), bg));
}

}  // namespace karafin::ui
