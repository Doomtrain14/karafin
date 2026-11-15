#pragma once

#include <functional>
#include <string>

#include "ui/modal.hpp"

namespace karafin::screens {

// Code entry with the D-pad (or left stick) or the on-screen arrows: each direction adds one step
// to the code, which is shown as dots (the newest briefly as its arrow). B or the delete button
// removes the last step; B cancels when nothing is entered.
//
// With a fixed `length` the code is submitted as soon as it is complete. With length 0 the code
// may be 4 or 6 steps: it is submitted at 6 steps, or with A / OK at 4. `onSubmit` gets the code
// ("U", "D", "L", "R" per step) and returns true to close, or false to shake, show `error` and
// start over. `lockout` (optional) returns the seconds entry is blocked for; it is checked when
// the pad opens and after each rejected code, and the pad counts down and ignores input meanwhile.
struct PinPadSpec {
    std::string title;
    std::string message;
    int length = 0;
    std::function<bool(const std::string& code, std::string& error)> onSubmit;
    std::function<int()> lockout;
    std::function<void()> onCancel;
};

class PinPad : public ui::Modal {
public:
    explicit PinPad(PinPadSpec spec);

    bool onInput(const ui::InputEvent& e) override;
    bool onTap(ui::Vec2 local) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    enum Button { Up, Down, Left, Right, Delete, Ok, ButtonCount };

    int slots() const { return spec_.length > 0 ? spec_.length : 6; }
    bool lockedOut() const { return lockLeft_ > 0; }
    bool canSubmitShort() const { return spec_.length == 0 && code_.size() == 4; }
    ui::Rect card() const;
    ui::Rect buttonRect(int button) const;
    void press(int button);
    void addStep(char step);
    void deleteStep();
    void cancel();
    void submit();

    PinPadSpec spec_;
    std::string code_;
    std::string error_;
    float shake_ = 0;
    float pop_ = 0;    // newest dot growing in
    float flash_ = 0;  // newest step shown as its arrow
    float lockLeft_ = 0;
    int pressed_ = -1;
    float pressT_ = 0;
    bool done_ = false;
};

}  // namespace karafin::screens
