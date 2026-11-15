#pragma once

// CPU image operations. Thread-safe and GL-free: run them on karafin::workers().

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "gfx/types.hpp"

namespace karafin::gfx {

struct Pixmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // tightly packed, 4 bytes per pixel

    bool empty() const { return width <= 0 || height <= 0; }
    size_t bytes() const { return rgba.size(); }
};

// Decodes JPEG (libjpeg-turbo, with DCT-domain downscaling toward the target), PNG (libpng,
// streamed and area-averaged toward the target row by row; interlaced PNGs go to stb_image),
// GIF/BMP (stb_image) and WebP (libwebp, scaled while decoding toward the target).
// `targetWidth/Height` (0 = unknown) only let the decoder skip detail; call fitTo() afterwards for
// the exact size. Returns straight alpha. Large decodes share a process-wide memory budget (they
// may wait for each other); stb_image / lossless WebP sources are capped lower than the others.
std::optional<Pixmap> decodeImage(std::string_view bytes, int targetWidth = 0, int targetHeight = 0,
                                  ImageFit fit = ImageFit::Cover);
// decodeImage() + premultiply() + fitTo() in one step, resampling straight from the decoder's
// buffer while the decode budget is still held, so the full-size image never outlives the call.
std::optional<Pixmap> decodeFitted(std::string_view bytes, int targetWidth, int targetHeight, ImageFit fit);

// Size an image of w x h should be scaled to for `fit` inside tw x th (never upscales).
void fitSize(int w, int h, int tw, int th, ImageFit fit, int& outW, int& outH);

// High quality area-average resample (bilinear when enlarging).
Pixmap resize(const Pixmap& src, int width, int height);
// resize() to fitSize(); returns the input unchanged when no downscale is needed.
Pixmap fitTo(Pixmap src, int targetWidth, int targetHeight, ImageFit fit);

// Gaussian approximation (3 box passes), in place. `sigma` in pixels.
void blur(Pixmap& image, float sigma);
void premultiply(Pixmap& image);
// Saturation-weighted average color (opaque). Good for placeholders and backdrop tints.
Color dominantColor(const Pixmap& image);

}  // namespace karafin::gfx
