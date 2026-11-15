#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ui/view.hpp"

namespace karafin::screens {

// SD card usage: Karafin downloads (accent), other data (grey), free space (track), with a
// legend line underneath.
class StorageBar : public ui::View {
public:
    StorageBar() = default;
    void setUsage(uint64_t downloadBytes, uint64_t freeBytes, uint64_t totalBytes);

    ui::Vec2 measure(ui::Vec2 available) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    uint64_t downloads_ = 0;
    uint64_t free_ = 0;
    uint64_t total_ = 0;
    std::array<std::string, 3> legend_;
    std::string totalText_;
    ui::Animated<float> dlFrac_{0.0f};
    ui::Animated<float> usedFrac_{0.0f};
};

}  // namespace karafin::screens
