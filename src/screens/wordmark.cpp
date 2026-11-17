#include "screens/wordmark.hpp"

#include <cmath>

#include "core/paths.hpp"
#include "screens/brand_art.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

Wordmark::Wordmark(float height, bool ink) : height_(height) {
    const float s = height / brand::kWordmarkRef;
    gfx::ImageRequest req;
    req.url = paths::romfs(ink ? "wordmark-ink.png" : "wordmark.png");
    req.width = static_cast<int>(std::ceil(brand::kWordmarkW * s * ui::displayScale()));
    req.height = static_cast<int>(std::ceil(brand::kWordmarkH * s * ui::displayScale()));
    req.fit = gfx::ImageFit::Fill;
    image_ = gfx::ImageCache::instance().load(req);
}

float Wordmark::width() const { return brand::kWordmarkW * height_ / brand::kWordmarkRef; }

void Wordmark::draw(gfx::Renderer& r, gfx::Vec2 topLeft) {
    const float s = height_ / brand::kWordmarkRef;
    if (const gfx::Texture* tex = image_ ? image_->texture() : nullptr)
        r.drawImage(*tex, {topLeft.x, topLeft.y + brand::kWordmarkOffsetY * s, brand::kWordmarkW * s, brand::kWordmarkH * s},
                    {.fit = gfx::ImageFit::Fill});
}

}  // namespace karafin::screens
