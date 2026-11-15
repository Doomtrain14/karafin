#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <GLES3/gl3.h>

namespace karafin::gfx {

enum class TextureFormat : uint8_t {
    RGBA8,  // premultiplied alpha expected by the renderer
    R8,     // single channel coverage (glyph atlas)
};

// GL texture handle + size. Main thread only (creation, update and destruction issue GL calls).
// Usually owned through std::shared_ptr; never let a worker thread hold the last reference.
class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // `pixels` may be null (allocates uninitialized storage). Rows are tightly packed.
    static std::shared_ptr<Texture> create(int width, int height, TextureFormat format, const void* pixels,
                                           bool linearFilter = true);
    // Wraps an existing GL texture. `owned` = delete it with this object.
    static std::shared_ptr<Texture> wrap(GLuint id, int width, int height, bool owned);

    void update(int x, int y, int width, int height, const void* pixels);

    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }
    TextureFormat format() const { return format_; }
    size_t bytes() const { return static_cast<size_t>(width_) * height_ * (format_ == TextureFormat::R8 ? 1 : 4); }

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
    TextureFormat format_ = TextureFormat::RGBA8;
    bool owned_ = true;
};

using TexturePtr = std::shared_ptr<Texture>;

// Offscreen color target (FBO + RGBA texture) sized in framebuffer pixels. Used for screen
// transitions and group opacity. Content is premultiplied; draw it with Renderer::drawTexture.
class RenderTarget {
public:
    RenderTarget() = default;
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // (Re)allocates when the size differs. Returns false if the FBO is incomplete.
    bool ensure(int pixelWidth, int pixelHeight);
    void release();

    bool valid() const { return fbo_ != 0; }
    GLuint fbo() const { return fbo_; }
    const TexturePtr& texture() const { return texture_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    GLuint fbo_ = 0;
    TexturePtr texture_;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace karafin::gfx
