#include "screens/keyboard.hpp"

#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "core/log.hpp"
#include "core/main_queue.hpp"

namespace karafin::screens {

std::optional<std::string> showKeyboard(const KeyboardOptions& options) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) {
        log::error("screens", "swkbdCreate failed: 0x{:x}", static_cast<unsigned>(rc));
        return std::nullopt;
    }
    switch (options.kind) {
        case KeyboardKind::Password: swkbdConfigMakePresetPassword(&kbd); break;
        case KeyboardKind::Url:
            swkbdConfigMakePresetDefault(&kbd);
            swkbdConfigSetType(&kbd, SwkbdType_QWERTY);
            break;
        case KeyboardKind::Text: swkbdConfigMakePresetDefault(&kbd); break;
    }
    if (!options.header.empty()) swkbdConfigSetHeaderText(&kbd, options.header.c_str());
    if (!options.sub.empty()) swkbdConfigSetSubText(&kbd, options.sub.c_str());
    if (!options.guide.empty()) swkbdConfigSetGuideText(&kbd, options.guide.c_str());
    if (!options.initial.empty()) swkbdConfigSetInitialText(&kbd, options.initial.c_str());
    if (!options.okLabel.empty()) swkbdConfigSetOkButtonText(&kbd, options.okLabel.c_str());
    swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(options.maxLength > 0 ? options.maxLength : 256));
    swkbdConfigSetStringLenMin(&kbd, options.allowEmpty ? 0 : 1);
    swkbdConfigSetBlurBackground(&kbd, 1);

    // UTF-8 can take up to 4 bytes per character.
    std::vector<char> out(static_cast<size_t>(options.maxLength > 0 ? options.maxLength : 256) * 4 + 1, '\0');
    rc = swkbdShow(&kbd, out.data(), out.size());
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return std::nullopt;  // cancelled
    return std::string(out.data());
#else
    (void)options;
    return std::nullopt;
#endif
}

void requestText(KeyboardOptions options, std::function<void(std::string)> onDone) {
    main_thread::post([options = std::move(options), onDone = std::move(onDone)] {
        auto text = showKeyboard(options);
        if (!text) return;
        if (text->empty() && !options.allowEmpty) return;
        if (onDone) onDone(std::move(*text));
    });
}

}  // namespace karafin::screens
