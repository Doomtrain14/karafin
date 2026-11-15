#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// System software keyboard (libnx swkbd) for text entry.
//
//   screens::requestText({.header = "Server address", .initial = "http://"},
//                        guard([this](std::string text) { connect(text); }));
namespace karafin::screens {

enum class KeyboardKind : uint8_t {
    Text,      // general text (search, user names)
    Url,       // QWERTY only, no auto-capitalisation: server addresses
    Password,  // masked input
};

struct KeyboardOptions {
    std::string header;    // large title above the field
    std::string sub;       // smaller text under the header
    std::string guide;     // placeholder shown while the field is empty
    std::string initial;   // pre-filled text
    std::string okLabel;   // confirm button text ("" = system default)
    KeyboardKind kind = KeyboardKind::Text;
    int maxLength = 256;
    bool allowEmpty = false;
};

// Shows the keyboard immediately and blocks until the user confirms or cancels. Main thread only.
// nullopt when cancelled (or the applet could not be shown).
std::optional<std::string> showKeyboard(const KeyboardOptions& options);

// Opens the keyboard at the start of the next frame (so the button press that triggered it has
// finished dispatching), then calls `onDone` with the text if the user confirmed.
void requestText(KeyboardOptions options, std::function<void(std::string)> onDone);

}  // namespace karafin::screens
