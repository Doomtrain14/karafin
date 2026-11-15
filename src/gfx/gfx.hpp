#pragma once

// gfx module umbrella header. See src/gfx/README.md for the API overview.
//
// Depends on: core. Must not depend on ui/screens/jellyfin/player/downloads.
// Threading: every GL call on the main thread; decode/blur on karafin::workers().

#include "gfx/icons.hpp"     // vector icon set
#include "gfx/image.hpp"     // async image loader + texture LRU + disk cache
#include "gfx/pixels.hpp"    // CPU decode / resize / blur / dominant color
#include "gfx/renderer.hpp"  // batched GLES3 renderer
#include "gfx/text.hpp"      // fonts, shaping, layout
#include "gfx/texture.hpp"   // Texture, RenderTarget
#include "gfx/types.hpp"     // Vec2, Rect, Color, Insets, Affine
