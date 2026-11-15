#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace karafin::ui {
namespace {

constexpr float kRepeatDelay = 0.35f;
constexpr float kRepeatSlow = 0.11f;
constexpr float kRepeatFast = 0.045f;
constexpr float kAccelTime = 1.6f;
constexpr int kStickPress = 16000;    // ~49% deflection
constexpr int kStickRelease = 11000;  // ~34%: hysteresis so a resting thumb does not chatter
constexpr int kTriggerPress = 12000;

bool repeats(Key b) {
    return b == Key::Up || b == Key::Down || b == Key::Left || b == Key::Right || b == Key::ZL ||
           b == Key::ZR;
}

// Raw button indices of the devkitPro switch joystick driver (SDL_sysjoystick.c,
// pad_mapping_default). Single Joy-Cons held sideways are remapped by the driver onto the same
// indices (0-3 = face positions, 12-15 = stick as d-pad).
std::optional<Key> fromSwitchJoyButton(int index) {
    static constexpr Key kKeys[] = {Key::A,  Key::B,  Key::X,  Key::Y,     Key::LStick, Key::RStick,
                                    Key::L,  Key::R,  Key::ZL, Key::ZR,    Key::Plus,   Key::Minus,
                                    Key::Left, Key::Up, Key::Right, Key::Down};
    if (index < 0 || index >= static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]))) return std::nullopt;
    return kKeys[index];
}

// Positional (Xbox layout) game-controller buttons -> Nintendo labels. Only used when the binding
// cannot be resolved to a raw joystick button.
std::optional<Key> fromPositional(uint8_t b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_B: return Key::A;
        case SDL_CONTROLLER_BUTTON_A: return Key::B;
        case SDL_CONTROLLER_BUTTON_Y: return Key::X;
        case SDL_CONTROLLER_BUTTON_X: return Key::Y;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Key::L;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Key::R;
        case SDL_CONTROLLER_BUTTON_START: return Key::Plus;
        case SDL_CONTROLLER_BUTTON_BACK: return Key::Minus;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Key::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Key::Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Key::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Key::Right;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return Key::LStick;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return Key::RStick;
        default: return std::nullopt;
    }
}

std::optional<Key> fromControllerButton(SDL_JoystickID which, uint8_t button) {
    if (SDL_GameController* gc = SDL_GameControllerFromInstanceID(which)) {
        const SDL_GameControllerButtonBind bind =
            SDL_GameControllerGetBindForButton(gc, static_cast<SDL_GameControllerButton>(button));
        const char* name = SDL_GameControllerName(gc);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && name && std::string_view(name) == "Switch Controller")
            if (auto k = fromSwitchJoyButton(bind.value.button)) return k;
    }
    return fromPositional(button);
}

bool isGameController(SDL_JoystickID which) { return SDL_GameControllerFromInstanceID(which) != nullptr; }

}  // namespace

std::optional<NavDir> navDirection(Key b) {
    switch (b) {
        case Key::Up: return NavDir::Up;
        case Key::Down: return NavDir::Down;
        case Key::Left: return NavDir::Left;
        case Key::Right: return NavDir::Right;
        default: return std::nullopt;
    }
}

const char* buttonName(Key b) {
    static const char* const kNames[] = {"A",  "B",     "X",  "Y",    "L",     "R",      "ZL",     "ZR", "+",
                                         "-",  "Up",    "Down", "Left", "Right", "LStick", "RStick"};
    const auto i = static_cast<size_t>(b);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

InputMapper::Pad* InputMapper::pad(SDL_JoystickID id) {
    Pad* freeSlot = nullptr;
    for (Pad& p : pads_) {
        if (p.id == id) return &p;
        if (p.id < 0 && !freeSlot) freeSlot = &p;
    }
    if (!freeSlot) return nullptr;
    *freeSlot = {};
    freeSlot->id = id;
    return freeSlot;
}

void InputMapper::press(Key b, std::vector<InputEvent>& out) {
    State& s = state_[static_cast<int>(b)];
    if (s.sources++ > 0) return;
    s.down = true;
    s.held = 0;
    s.nextRepeat = kRepeatDelay;
    out.push_back({b, InputAction::Press, 0});
}

void InputMapper::release(Key b, std::vector<InputEvent>& out) {
    State& s = state_[static_cast<int>(b)];
    if (s.sources == 0) return;
    if (--s.sources > 0) return;
    s.down = false;
    out.push_back({b, InputAction::Release, s.held});
}

void InputMapper::padKey(Pad& p, Key k, bool down, std::vector<InputEvent>& out) {
    const uint32_t bit = 1u << static_cast<int>(k);
    if (down == ((p.keys & bit) != 0)) return;
    if (down) {
        p.keys |= bit;
        press(k, out);
    } else {
        p.keys &= ~bit;
        release(k, out);
    }
}

// Left stick -> one held direction. The dominant axis wins (no diagonal double moves); switching
// axes needs a clear lead, and releasing needs to fall below a lower threshold (hysteresis).
void InputMapper::updateStick(Pad& p, std::vector<InputEvent>& out) {
    const int ax = std::abs(p.lx), ay = std::abs(p.ly);
    auto dirKey = [&](bool horizontal) {
        return static_cast<int>(horizontal ? (p.lx < 0 ? Key::Left : Key::Right) : (p.ly < 0 ? Key::Up : Key::Down));
    };
    int next = p.stickKey;
    if (next < 0) {
        if (std::max(ax, ay) > kStickPress) next = dirKey(ax >= ay);
    } else {
        const auto cur = static_cast<Key>(next);
        const bool horizontal = cur == Key::Left || cur == Key::Right;
        const int along = horizontal ? ax : ay;
        const int across = horizontal ? ay : ax;
        const bool sameSide = horizontal ? ((p.lx < 0) == (cur == Key::Left)) : ((p.ly < 0) == (cur == Key::Up));
        if (across > kStickPress && across > along + along / 4) next = dirKey(!horizontal);
        else if (!sameSide && along > kStickPress) next = dirKey(horizontal);
        else if (!sameSide || along < kStickRelease) next = -1;
    }
    if (next == p.stickKey) return;
    if (p.stickKey >= 0) release(static_cast<Key>(p.stickKey), out);
    p.stickKey = next;
    if (next >= 0) press(static_cast<Key>(next), out);
}

bool InputMapper::handle(const SDL_Event& e, std::vector<InputEvent>& out) {
    switch (e.type) {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            Pad* p = pad(e.cbutton.which);
            if (!p) return true;
            if (auto k = fromControllerButton(e.cbutton.which, e.cbutton.button))
                padKey(*p, *k, e.type == SDL_CONTROLLERBUTTONDOWN, out);
            return true;
        }
        case SDL_CONTROLLERAXISMOTION: {
            Pad* p = pad(e.caxis.which);
            if (!p) return true;
            const int v = e.caxis.value;
            switch (e.caxis.axis) {
                case SDL_CONTROLLER_AXIS_LEFTX:
                    p->lx = v;
                    updateStick(*p, out);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    p->ly = v;
                    updateStick(*p, out);
                    break;
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
                    const int which = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : 1;
                    const bool down = v > kTriggerPress;
                    if (p->trigger[which] == down) break;
                    p->trigger[which] = down;
                    const Key k = which == 0 ? Key::ZL : Key::ZR;
                    if (down) press(k, out);
                    else release(k, out);
                    break;
                }
                default: break;
            }
            return true;
        }
        // Fallback for joysticks without a game-controller mapping (raw switch layout).
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP: {
            if (isGameController(e.jbutton.which)) return false;
            Pad* p = pad(e.jbutton.which);
            if (!p) return true;
            if (auto k = fromSwitchJoyButton(e.jbutton.button)) padKey(*p, *k, e.type == SDL_JOYBUTTONDOWN, out);
            return true;
        }
        case SDL_JOYAXISMOTION: {
            if (isGameController(e.jaxis.which)) return false;
            Pad* p = pad(e.jaxis.which);
            if (!p) return true;
            if (e.jaxis.axis == 0) p->lx = e.jaxis.value;
            else if (e.jaxis.axis == 1) p->ly = e.jaxis.value;
            else return true;
            updateStick(*p, out);
            return true;
        }
        default: return false;
    }
}

void InputMapper::update(float dt, std::vector<InputEvent>& out) {
    for (int i = 0; i < static_cast<int>(Key::Count); ++i) {
        State& s = state_[i];
        if (!s.down) continue;
        s.held += dt;
        const auto b = static_cast<Key>(i);
        if (!repeats(b)) continue;
        if (s.held >= s.nextRepeat) {
            const float accel = std::clamp((s.held - kRepeatDelay) / kAccelTime, 0.0f, 1.0f);
            const float interval = kRepeatSlow + (kRepeatFast - kRepeatSlow) * accel * accel;
            s.nextRepeat = std::max(s.nextRepeat + interval, s.held + interval * 0.5f);
            out.push_back({b, InputAction::Repeat, s.held});
        }
    }
}

void InputMapper::reset(std::vector<InputEvent>* releases) {
    for (int i = 0; i < static_cast<int>(Key::Count); ++i) {
        State& s = state_[i];
        if (s.down && releases) releases->push_back({static_cast<Key>(i), InputAction::Release, s.held});
        s = {};
    }
    // Keep the axis values (the stick may still be deflected) but forget what they held; the next
    // axis event re-presses if still deflected.
    for (Pad& p : pads_) {
        p.keys = 0;
        p.stickKey = -1;
        p.trigger[0] = p.trigger[1] = false;
    }
}

}  // namespace karafin::ui
