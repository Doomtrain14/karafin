#pragma once

// Controller input as semantic Switch buttons, with key repeat and stick-to-D-pad mapping.
//
// The devkitPro SDL2 port exposes 8 joysticks ("Switch Controller"; #0 = handheld + player 1)
// whose raw button indices are the physical buttons (0 A, 1 B, 2 X, 3 Y, 4/5 stick clicks,
// 6 L, 7 R, 8 ZL, 9 ZR, 10 +, 11 -, 12-15 d-pad left/up/right/down). Its game-controller mapping
// is positional (Xbox layout: SDL "A" = bottom = Nintendo B). InputMapper resolves every
// controller button back to the raw joystick button through the mapping, so Key::A is always the
// physical right-hand "confirm" button regardless of the mapping or label hints. Joysticks
// without a mapping are read from raw SDL_JOY* events with the same layout.
//
// Conventions: A confirm, B back, X/Y contextual, L/R switch tabs, ZL/ZR page up/down,
// + menu, - secondary menu. Directions repeat with acceleration while held.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <SDL.h>

namespace karafin::ui {

enum class Key : uint8_t {
    A, B, X, Y,
    L, R, ZL, ZR,
    Plus, Minus,
    Up, Down, Left, Right,
    LStick, RStick,
    Count,
};

enum class InputAction : uint8_t { Press, Repeat, Release };

enum class NavDir : uint8_t { Up, Down, Left, Right };

struct InputEvent {
    Key button = Key::A;
    InputAction action = InputAction::Press;
    float held = 0;  // seconds the button has been held (repeats)

    bool pressed() const { return action == InputAction::Press; }
    bool pressedOrRepeat() const { return action != InputAction::Release; }
    bool is(Key b) const { return button == b && action != InputAction::Release; }
};

// Key + label shown in the hint bar.
struct Hint {
    Key button;
    std::string label;
};

std::optional<NavDir> navDirection(Key b);
const char* buttonName(Key b);

class InputMapper {
public:
    // Translates one SDL event (controller / raw joystick buttons and axes) into zero or more
    // InputEvents. Returns true if the event is an input event this mapper understands.
    bool handle(const SDL_Event& e, std::vector<InputEvent>& out);
    // Generates repeats; call once per frame.
    void update(float dt, std::vector<InputEvent>& out);
    bool held(Key b) const { return state_[static_cast<int>(b)].down; }
    // Drops all held state (focus loss, screen change while holding).
    void reset(std::vector<InputEvent>* releases = nullptr);

private:
    static constexpr int kMaxPads = 8;
    struct State {
        bool down = false;
        int sources = 0;  // pads / d-pad + stick may hold the same key
        float held = 0;
        float nextRepeat = 0;
    };
    struct Pad {
        SDL_JoystickID id = -1;
        uint32_t keys = 0;  // keys currently held by this pad (bit per Key)
        int lx = 0, ly = 0;
        int stickKey = -1;  // Key held by the left stick, -1 = none
        bool trigger[2] = {false, false};
    };
    Pad* pad(SDL_JoystickID id);
    void padKey(Pad& p, Key k, bool down, std::vector<InputEvent>& out);
    void updateStick(Pad& p, std::vector<InputEvent>& out);
    void press(Key b, std::vector<InputEvent>& out);
    void release(Key b, std::vector<InputEvent>& out);

    State state_[static_cast<int>(Key::Count)];
    Pad pads_[kMaxPads];
};

}  // namespace karafin::ui
