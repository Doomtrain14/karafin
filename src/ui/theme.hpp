#pragma once

// Design tokens. Every screen and widget should take colors, spacing, radii, type and motion
// from here so the app stays coherent. Values are logical pixels in the 1280x720 space.

#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "gfx/types.hpp"

namespace karafin::ui::theme {

using gfx::Color;

// ---- Color --------------------------------------------------------------------------------
namespace color {
// Surfaces, darkest to lightest. Slightly blue-shifted neutral blacks.
inline constexpr Color background = Color::hex(0x0A0A0F);
inline constexpr Color backgroundRaised = Color::hex(0x111118);
inline constexpr Color surface = Color::hex(0x1A1A24);
inline constexpr Color surfaceHigh = Color::hex(0x24242F);
inline constexpr Color surfaceHighest = Color::hex(0x30303D);
inline constexpr Color skeleton = Color::hex(0x1E1E29);

// Text.
inline constexpr Color textPrimary = Color::hex(0xF5F5FA);
inline constexpr Color textSecondary = Color(0.96f, 0.96f, 0.98f, 0.66f);
inline constexpr Color textTertiary = Color(0.96f, 0.96f, 0.98f, 0.42f);
inline constexpr Color textOnAccent = Color::hex(0xFFFFFF);
inline constexpr Color textOnLight = Color::hex(0x0B0B12);  // text on a focused (white) button

// Brand: Karafin violet with an indigo partner for gradients.
inline constexpr Color accent = Color::hex(0x8B6CFF);
inline constexpr Color accentBright = Color::hex(0xA993FF);
inline constexpr Color accentDeep = Color::hex(0x5B4BE0);
inline constexpr Color accentGlow = Color(0.545f, 0.424f, 1.0f, 0.55f);

// Feedback.
inline constexpr Color success = Color::hex(0x3DD68C);
inline constexpr Color warning = Color::hex(0xFFB547);
inline constexpr Color danger = Color::hex(0xFF5A5F);
inline constexpr Color info = Color::hex(0x5AB4FF);

// Lines, overlays, focus.
inline constexpr Color separator = Color(1, 1, 1, 0.08f);
inline constexpr Color stroke = Color(1, 1, 1, 0.12f);
inline constexpr Color focusRing = Color(1, 1, 1, 0.92f);
inline constexpr Color scrim = Color(0, 0, 0, 0.62f);         // behind dialogs / sheets
inline constexpr Color shadow = Color(0, 0, 0, 0.55f);
inline constexpr Color glassFill = Color(0.12f, 0.12f, 0.17f, 0.86f);  // sheets, toasts, OSD panels
inline constexpr Color buttonSecondary = Color(1, 1, 1, 0.10f);
inline constexpr Color buttonSecondaryFocused = Color(1, 1, 1, 0.94f);
inline constexpr Color progressTrack = Color(1, 1, 1, 0.22f);
}  // namespace color

// Accent gradient used for primary buttons, progress fills and highlights.
inline constexpr gfx::Gradient accentGradient = gfx::Gradient::horizontal(color::accent, color::accentDeep);

// ---- Spacing / layout ---------------------------------------------------------------------
namespace space {
inline constexpr float xxs = 4;
inline constexpr float xs = 8;
inline constexpr float sm = 12;
inline constexpr float md = 16;
inline constexpr float lg = 24;
inline constexpr float xl = 32;
inline constexpr float xxl = 48;
inline constexpr float xxxl = 64;
}  // namespace space

namespace layout {
inline constexpr float screenWidth = 1280;
inline constexpr float screenHeight = 720;
inline constexpr float marginX = 56;        // horizontal safe margin for content
inline constexpr float marginTop = 40;
inline constexpr float hintBarHeight = 44;  // reserved at the bottom when the hint bar is shown
inline constexpr float rowSpacing = 34;     // between carousels
inline constexpr float cardSpacing = 18;    // between cards in a row
inline constexpr float focusOverscan = 22;  // room around rows for focused cards to grow into
}  // namespace layout

// ---- Radii --------------------------------------------------------------------------------
namespace radius {
inline constexpr float xs = 4;
inline constexpr float sm = 8;
inline constexpr float md = 12;
inline constexpr float lg = 16;
inline constexpr float xl = 24;
inline constexpr float pill = 999;
}  // namespace radius

// ---- Typography ---------------------------------------------------------------------------
namespace type {
using gfx::FontWeight;
using gfx::TextStyle;
inline constexpr TextStyle hero{44, FontWeight::Display, 1.08f, -0.6f};
inline constexpr TextStyle largeTitle{34, FontWeight::Display, 1.12f, -0.4f};
inline constexpr TextStyle title1{28, FontWeight::Display, 1.15f, -0.3f};  // Display cut from 28 px up
inline constexpr TextStyle title2{22, FontWeight::Bold, 1.2f, -0.2f};
inline constexpr TextStyle title3{19, FontWeight::SemiBold, 1.25f, -0.1f};
inline constexpr TextStyle headline{16, FontWeight::SemiBold, 1.3f, 0.0f};
inline constexpr TextStyle body{16, FontWeight::Regular, 1.45f, 0.0f};
inline constexpr TextStyle callout{15, FontWeight::Medium, 1.35f, 0.0f};
inline constexpr TextStyle subhead{14, FontWeight::Regular, 1.35f, 0.0f};
inline constexpr TextStyle caption{13, FontWeight::Medium, 1.3f, 0.1f};
inline constexpr TextStyle footnote{12, FontWeight::Medium, 1.3f, 0.2f};
inline constexpr TextStyle overline{11, FontWeight::SemiBold, 1.3f, 1.2f};  // ALL CAPS labels
inline constexpr TextStyle button{15, FontWeight::SemiBold, 1.0f, 0.1f};
}  // namespace type

// ---- Motion -------------------------------------------------------------------------------
namespace motion {
// Spring presets: {response (s, ~period), damping ratio (1 = critical, <1 bouncy)}.
struct SpringPreset {
    float response;
    float damping;
};
inline constexpr SpringPreset focus{0.30f, 0.72f};      // card / button focus scale (gentle overshoot)
inline constexpr SpringPreset standard{0.38f, 1.0f};    // general property changes
inline constexpr SpringPreset snappy{0.22f, 1.0f};      // highlights following focus
inline constexpr SpringPreset scroll{0.48f, 1.0f};      // scroll-into-view
inline constexpr SpringPreset gentle{0.60f, 1.0f};      // backdrops, large surfaces
inline constexpr SpringPreset bouncy{0.42f, 0.62f};     // playful accents (toasts)

inline constexpr float fast = 0.15f;
inline constexpr float normal = 0.25f;
inline constexpr float slow = 0.40f;
inline constexpr float screenTransition = 0.42f;
inline constexpr float imageFade = 0.35f;
}  // namespace motion

// ---- Component metrics --------------------------------------------------------------------
namespace card {
inline constexpr float posterWidth = 148;   // 2:3
inline constexpr float posterHeight = 222;
inline constexpr float wideWidth = 268;     // 16:9
inline constexpr float wideHeight = 151;
inline constexpr float squareSize = 160;
inline constexpr float radius = 12;
inline constexpr float focusScale = 1.08f;
inline constexpr float pressScale = 0.96f;
}  // namespace card

namespace button {
inline constexpr float height = 46;
inline constexpr float paddingX = 24;
inline constexpr float iconSize = 20;
inline constexpr float iconButtonSize = 48;
inline constexpr float focusScale = 1.06f;
}  // namespace button

struct Shadow {
    float blur;
    float offsetY;
    float alpha;
};
inline constexpr Shadow shadowRest{14, 6, 0.40f};
inline constexpr Shadow shadowFocus{34, 18, 0.70f};

}  // namespace karafin::ui::theme
