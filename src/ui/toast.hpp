#pragma once

// Transient notifications: glassy pills that spring up above the hint bar and stack (max 3).
//
//   ui::toast("Download complete", ui::ToastKind::Success);
//   ui::toast("Server unreachable", ui::ToastKind::Error, gfx::Icon::Wifi, 4.0f);

#include <deque>
#include <string>

#include "gfx/icons.hpp"
#include "gfx/renderer.hpp"
#include "ui/anim.hpp"

namespace karafin::ui {

enum class ToastKind : uint8_t { Info, Success, Warning, Error };

class ToastHost {
public:
    void show(std::string text, ToastKind kind = ToastKind::Info, gfx::Icon icon = gfx::Icon::None,
              float duration = 2.8f);
    void update(float dt);
    // `bottom`: y the stack rests on (above the hint bar).
    void render(gfx::Renderer& r, float screenWidth, float bottom);
    void clear() { toasts_.clear(); }

private:
    struct Toast {
        std::string text;
        ToastKind kind;
        gfx::Icon icon;
        float remaining;
        Animated<float> appear{0.0f};
        Animated<float> slot{0.0f};  // stack position (0 = bottom)
        bool leaving = false;
    };
    std::deque<Toast> toasts_;
};

}  // namespace karafin::ui
