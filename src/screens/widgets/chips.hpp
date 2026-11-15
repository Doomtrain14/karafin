#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/icons.hpp"
#include "ui/view.hpp"

namespace karafin::screens {

enum class ChipStyle : uint8_t {
    Text,     // plain secondary text (year, runtime)
    Outline,  // thin border (rating, resolution)
    Filled,   // translucent fill (codec, audio)
    Accent,   // accent fill (HDR, Dolby Vision, 4K)
};

struct Chip {
    std::string text;
    ChipStyle style = ChipStyle::Outline;
    gfx::Icon icon = gfx::Icon::None;
};

// One line of metadata chips: "2019  [PG-13]  2h 11m  * 7.8  [4K] [HDR10] [5.1]".
class ChipRow : public ui::View {
public:
    ChipRow() = default;
    void setChips(std::vector<Chip> chips);
    bool empty() const { return chips_.empty(); }

    ui::Vec2 measure(ui::Vec2 available) override;
    void draw(ui::Renderer& r) override;

private:
    float chipWidth(const Chip& c) const;
    std::vector<Chip> chips_;
};

}  // namespace karafin::screens
