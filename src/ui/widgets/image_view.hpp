#pragma once

#include "gfx/icons.hpp"
#include "gfx/image.hpp"
#include "ui/theme.hpp"
#include "ui/view.hpp"

namespace karafin::ui {

// Async image with skeleton shimmer while loading and a fade-in when ready.
//
//   auto* img = add<ImageView>();
//   img->setRadius(theme::radius::md);
//   img->setUrl(url, authHeaders);   // decode size = frame size * display scale (auto)
//
// The request is issued once the view has a size (after layout) and re-issued at a higher
// resolution if the display scale grows (handheld -> docked).
class ImageView : public View {
public:
    ImageView();

    // width/height of 0 in the request are filled from the frame size * display scale.
    void setRequest(gfx::ImageRequest request);
    void setUrl(std::string url, http::Headers headers = {});
    void setImage(gfx::ImagePtr image);
    void clear();
    const gfx::ImagePtr& image() const { return image_; }
    bool loaded() const { return image_ && image_->ready(); }

    void setFit(gfx::ImageFit fit) { style_.fit = fit; }
    void setRadius(float r) { style_.radius = r; }
    void setFocusPoint(Vec2 p) { style_.focus = p; }
    void setTint(Color c) { style_.tint = c; }
    void setPlaceholderColor(Color c) { placeholder_ = c; }
    // Icon drawn centered on the placeholder when there is no image / it failed.
    void setFallbackIcon(gfx::Icon icon) { fallbackIcon_ = icon; }
    void setShimmer(bool v) { shimmer_ = v; }
    void setFadeDuration(float s) { fadeDuration_ = s; }

    std::function<void(gfx::ImageRef&)> onLoaded;

    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    void issue();

    gfx::ImagePtr image_;
    gfx::ImageRequest request_;
    gfx::ImageStyle style_;
    Color placeholder_ = theme::color::skeleton;
    gfx::Icon fallbackIcon_ = gfx::Icon::None;
    Animated<float> reveal_{0.0f};
    float shimmerPhase_ = 0;
    float requestedScale_ = 0;
    float fadeDuration_ = theme::motion::imageFade;
    bool pending_ = false;
    bool sawLoading_ = false;
    bool notified_ = false;
    bool shimmer_ = true;
    bool autoSize_ = false;
};

}  // namespace karafin::ui
