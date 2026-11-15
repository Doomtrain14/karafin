#pragma once

// Flex-like stack layout.
//
//   auto* col = add<Box>(Axis::Vertical);
//   col->setPadding(Insets::all(24)).setSpacing(12).setAlign(Align::Start);
//   col->add<Label>("Title", theme::type::title2);
//   auto* row = col->add<Box>(Axis::Horizontal);
//   row->setSpacing(16);
//   row->add<Button>("Play", gfx::Icon::Play)->setFlex(0);
//
// Main axis: children take their measured/preferred size plus margins; remaining space goes to
// children with flex > 0 (proportionally). Cross axis: Align::Stretch fills, otherwise the child's
// measured size aligned start/center/end. A Box with an automatic preferred size measures to fit
// its children.

#include "ui/view.hpp"

namespace karafin::ui {

enum class Axis : uint8_t { Horizontal, Vertical };
enum class Align : uint8_t { Start, Center, End, Stretch };
enum class Justify : uint8_t { Start, Center, End, SpaceBetween };

class Box : public View {
public:
    explicit Box(Axis axis = Axis::Vertical) : axis_(axis) {}

    Box& setAxis(Axis a);
    Box& setPadding(const Insets& p);
    Box& setSpacing(float s);
    Box& setAlign(Align a);
    Box& setJustify(Justify j);

    Axis axis() const { return axis_; }
    const Insets& padding() const { return padding_; }
    float spacing() const { return spacing_; }

    Vec2 measure(Vec2 available) override;
    void layout() override;

private:
    Axis axis_;
    Insets padding_;
    float spacing_ = 0;
    Align align_ = Align::Stretch;
    Justify justify_ = Justify::Start;
};

// Empty view: fixed gap (size) or flexible space (flex > 0) inside a Box.
class Spacer : public View {
public:
    explicit Spacer(float size = 0, float flex = 0) {
        setPreferredSize(size, size);
        setFlex(flex);
    }
};

}  // namespace karafin::ui
