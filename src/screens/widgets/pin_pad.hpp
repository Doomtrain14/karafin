#pragma once

#include <functional>
#include <string>

#include "ui/modal.hpp"

namespace karafin::screens {

// Code entry with the D-pad (or left stick): each direction adds one step to the code, which is
// shown as dots. B deletes the last step, or cancels when nothing is entered.
//
// With a fixed `length` the code is submitted as soon as it is complete. With length 0 the code
// may be 4 or 6 steps: it is submitted at 6 steps, or with A at 4. `onSubmit` gets the code
// ("U", "D", "L", "R" per step) and returns true to close, or false to shake, show `error` and
// start over.
struct PinPadSpec {
    std::string title;
    std::string message;
    int length = 0;
    std::function<bool(const std::string& code, std::string& error)> onSubmit;
};

class PinPad : public ui::Modal {
public:
    explicit PinPad(PinPadSpec spec);

    bool onInput(const ui::InputEvent& e) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    int slots() const { return spec_.length > 0 ? spec_.length : 6; }
    void submit();

    PinPadSpec spec_;
    std::string code_;
    std::string error_;
    float shake_ = 0;
    float pop_ = 0;  // newest dot growing in
    bool done_ = false;
};

}  // namespace karafin::screens
