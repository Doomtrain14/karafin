#pragma once

// Text: FreeType rasterization into a shared glyph atlas, HarfBuzz shaping (kerning, ligatures),
// bundled Inter faces with Nintendo shared-font fallback (CJK, Hangul, Nintendo symbols).
//
//   auto layout = gfx::text::layout("Hello", {.size = 20, .weight = gfx::FontWeight::SemiBold},
//                                   {.maxWidth = 300, .maxLines = 2});
//   renderer.drawText(*layout, {x, y}, color);
//
// Layouts are cached (keyed by text + style + options + display scale) so calling layout() every
// frame for the same string is cheap. Main thread only.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/types.hpp"

namespace karafin::gfx {

class Texture;

enum class FontWeight : uint8_t {
    Regular,
    Medium,
    SemiBold,
    Bold,
    Display,  // InterDisplay Bold: tighter design for large titles (>= 28 px)
};

struct TextStyle {
    float size = 16.0f;                     // logical px (em size)
    FontWeight weight = FontWeight::Regular;
    float lineHeight = 1.3f;                // multiple of size
    float tracking = 0.0f;                  // extra logical px after every glyph (negative tightens)
};

enum class TextAlign : uint8_t { Left, Center, Right };

struct TextOptions {
    float maxWidth = 0.0f;  // 0 = unbounded (no wrapping / truncation)
    int maxLines = 1;       // 1 = single line; 0 = unlimited. Wrapping needs maxWidth > 0.
    TextAlign align = TextAlign::Left;  // alignment of each line inside the layout width
    bool ellipsis = true;   // append "…" when truncated
};

struct PlacedGlyph {
    uint16_t face = 0;
    uint32_t glyph = 0;
    float x = 0;  // pen position in framebuffer pixels, relative to the layout origin
    float y = 0;  // baseline in framebuffer pixels, relative to the layout origin
};

class TextLayout {
public:
    float width = 0;          // logical px (widest line; == maxWidth for Center/Right aligned wrapped text)
    float height = 0;         // logical px (lineCount * lineHeight)
    float lineHeight = 0;     // logical px
    float firstBaseline = 0;  // logical px from the top
    int lineCount = 0;
    bool truncated = false;
    float pixelSize = 0;      // font size in framebuffer pixels
    float scale = 1;          // display scale the layout was made for
    std::vector<PlacedGlyph> glyphs;
};

using TextLayoutPtr = std::shared_ptr<const TextLayout>;

namespace text {

// Loads romfs:/fonts/*.ttf and the shared system fonts. Called by Renderer::init().
bool init();
void shutdown();

// Layout for the current display scale (Renderer::beginFrame keeps it in sync).
TextLayoutPtr layout(std::string_view text, const TextStyle& style, const TextOptions& options = {});
Vec2 measure(std::string_view text, const TextStyle& style, const TextOptions& options = {});
// Line advance in logical px for a style.
float lineHeight(const TextStyle& style);
// Ascender / cap height in logical px (useful for optical vertical centering).
float capHeight(const TextStyle& style);

// Drops cached layouts unused for a while; frees the atlas when the display scale changes.
void endFrame();
void setDisplayScale(float scale);
float displayScale();

// Glyph atlas lookup used by the renderer (rasterizes on first use).
struct GlyphQuad {
    const Texture* texture = nullptr;  // R8 atlas page; null for empty glyphs (spaces)
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    int width = 0, height = 0;  // bitmap size in px
    int left = 0, top = 0;      // bearing in px (top is distance above the baseline)
};
const GlyphQuad& glyph(uint16_t face, uint32_t glyphIndex, float pixelSize, int subpixel);
inline constexpr int kSubpixelSteps = 4;
// New glyphs are rasterized into a CPU copy of their atlas page; this uploads the dirty
// rectangle of each page (one glTexSubImage2D per page instead of one per glyph). The renderer
// calls it before every draw call.
void flushAtlas();

}  // namespace text
}  // namespace karafin::gfx
