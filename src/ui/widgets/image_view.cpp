#include "ui/widgets/image_view.hpp"

#include <cmath>

#include "ui/root.hpp"

namespace karafin::ui {

ImageView::ImageView() { setCullMargin(8); }

void ImageView::setRequest(gfx::ImageRequest request) {
    // Rebinding the same artwork (row refreshes, recycled grid cells) must not restart the fade.
    if ((image_ || pending_) && !request.url.empty() && request.url == request_.url &&
        request.width == request_.width && request.height == request_.height && request.fit == request_.fit &&
        request.blur == request_.blur && request.cacheKey == request_.cacheKey && !(image_ && image_->failed()))
        return;
    request_ = std::move(request);
    autoSize_ = request_.width <= 0 && request_.height <= 0;
    pending_ = !request_.url.empty();
    image_.reset();
    reveal_.set(0.0f);
    sawLoading_ = notified_ = false;
    if (!autoSize_ && pending_) issue();
}

void ImageView::setUrl(std::string url, http::Headers headers) {
    gfx::ImageRequest req;
    req.url = std::move(url);
    req.headers = std::move(headers);
    req.fit = style_.fit;
    setRequest(std::move(req));
}

void ImageView::setImage(gfx::ImagePtr image) {
    request_ = {};
    pending_ = false;
    image_ = std::move(image);
    const bool ready = image_ && image_->ready();
    reveal_.set(ready ? 1.0f : 0.0f);
    sawLoading_ = !ready;
    notified_ = false;
}

void ImageView::clear() {
    request_ = {};
    pending_ = false;
    image_.reset();
    reveal_.set(0.0f);
}

void ImageView::issue() {
    gfx::ImageRequest req = request_;
    const float s = displayScale();
    if (autoSize_) {
        req.width = static_cast<int>(std::lround(width() * s));
        req.height = static_cast<int>(std::lround(height() * s));
    }
    requestedScale_ = s;
    pending_ = false;
    image_ = gfx::ImageCache::instance().load(req);
    const bool ready = image_->ready();
    reveal_.set(ready ? 1.0f : 0.0f);
    sawLoading_ = !ready;
    notified_ = false;
}

void ImageView::update(float dt) {
    if (pending_ && (!autoSize_ || (width() > 1 && height() > 1))) issue();
    // Sharper decode after switching to docked (1080p).
    if (autoSize_ && image_ && !request_.url.empty() && displayScale() > requestedScale_ + 0.01f) issue();

    if (image_) {
        if (image_->ready() && reveal_.target() < 1.0f) {
            if (sawLoading_) reveal_.tweenTo(1.0f, fadeDuration_, ease::outCubic);
            else reveal_.set(1.0f);
        }
        if (!notified_ && (image_->ready() || image_->failed())) {
            notified_ = true;
            if (onLoaded) onLoaded(*image_);
        }
    }
    reveal_.update(dt);
    // Shared clock: every skeleton on screen sweeps in sync instead of drifting per view.
    shimmerPhase_ = static_cast<float>(std::fmod(now() / 1.4, 1.0));
    View::update(dt);
}

void ImageView::draw(Renderer& r) {
    const Rect b = bounds();
    const gfx::Texture* tex = image_ ? image_->texture() : nullptr;
    const float reveal = tex ? reveal_.value() : 0.0f;
    if (reveal < 1.0f) {
        const bool loading = image_ ? image_->loading() : pending_;
        if (loading && shimmer_) r.fillShimmer(b, style_.radius, placeholder_, shimmerPhase_);
        else r.fillRoundRect(b, style_.radius, placeholder_);
        if (!loading && fallbackIcon_ != gfx::Icon::None) {
            const float s = std::min(b.w, b.h) * 0.3f;
            gfx::drawIcon(r, fallbackIcon_, {b.center().x - s * 0.5f, b.center().y - s * 0.5f, s, s},
                          theme::color::textTertiary);
        }
    }
    if (tex && reveal > 0.0f) {
        gfx::ImageStyle st = style_;
        st.tint = style_.tint.mulAlpha(reveal);
        r.drawImage(*tex, b, st);
    }
}

}  // namespace karafin::ui
