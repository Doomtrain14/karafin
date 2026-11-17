#pragma once

#include <functional>
#include <string>

#include "gfx/icons.hpp"
#include "gfx/image.hpp"
#include "ui/view.hpp"

namespace karafin::screens {

// Round user avatar with the name underneath (login "Who's watching?" grid). Shows the
// initial on a colored disc when there is no picture, or an icon (e.g. Plus for "Other user").
class AvatarTile : public ui::View {
public:
    static constexpr float kSize = 116;

    AvatarTile(std::string name, std::string imageUrl, gfx::Icon icon = gfx::Icon::None);

    void setCaption(std::string caption) { caption_ = std::move(caption); }
    void setMarked(bool v) { marked_ = v; }


    ui::Vec2 measure(ui::Vec2 available) override;
    void onFocus() override;
    void onBlur() override;
    bool activate() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    std::string name_;
    std::string caption_;
    gfx::Icon icon_;
    gfx::ImagePtr image_;
    ui::Color color_;
    bool marked_ = false;
    ui::Animated<float> focus_{0.0f};
    ui::Animated<float> reveal_{0.0f};
};

}  // namespace karafin::screens
