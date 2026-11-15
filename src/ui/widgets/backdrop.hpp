#pragma once

#include "gfx/image.hpp"
#include "ui/theme.hpp"
#include "ui/view.hpp"

namespace karafin::ui {

// Full-bleed hero art that crossfades when the image changes, with scrims that melt it into the
// background (bottom and left) so text on top stays legible. Pair with ImageRequest::blur for a
// soft ambient backdrop.
//
//   auto* bg = add<Backdrop>();
//   bg->setFrame({0, 0, 1280, 520});
//   gfx::ImageRequest req; req.url = backdropUrl; req.width = 640; req.height = 360; req.blur = 6;
//   bg->setRequest(req);
class Backdrop : public View {
public:
    Backdrop();

    void setRequest(const gfx::ImageRequest& request);
    void setImage(gfx::ImagePtr image);
    void clear();
    // Opacity of the art itself (scrims always blend to the background color).
    void setIntensity(float a) { intensity_ = a; }
    void setScrims(bool bottom, bool left) {
        bottomScrim_ = bottom;
        leftScrim_ = left;
    }
    // Tint wash using the image's dominant color (ambient color behind the UI).
    void setAmbientTint(bool v) { ambient_ = v; }

    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    struct Layer {
        gfx::ImagePtr image;
        Animated<float> alpha{0.0f};
    };
    Layer current_;
    Layer previous_;
    Animated<Color> tint_{theme::color::background};
    float intensity_ = 0.85f;
    bool bottomScrim_ = true;
    bool leftScrim_ = true;
    bool ambient_ = true;
};

}  // namespace karafin::ui
