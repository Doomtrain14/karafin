#include "gfx/texture.hpp"

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "gfx/renderer.hpp"

namespace karafin::gfx {
namespace {
// Texture creation/updates bind on a unit the renderer never samples from, so uploads in the
// middle of a batch cannot disturb the batch's bound slots (units 0-7).
void bindScratch(GLuint id) {
    glActiveTexture(GL_TEXTURE0 + 8);
    glBindTexture(GL_TEXTURE_2D, id);
}
}  // namespace

Texture::~Texture() {
    if (id_ && owned_) {
        main_thread::assertCurrent("Texture::~Texture");
        if (Renderer::available()) Renderer::instance().forgetTexture(id_);
        glDeleteTextures(1, &id_);
    }
}

std::shared_ptr<Texture> Texture::create(int width, int height, TextureFormat format, const void* pixels,
                                         bool linearFilter) {
    main_thread::assertCurrent("Texture::create");
    auto tex = std::make_shared<Texture>();
    tex->width_ = width;
    tex->height_ = height;
    tex->format_ = format;
    glGenTextures(1, &tex->id_);
    bindScratch(tex->id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linearFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linearFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (format == TextureFormat::R8)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return tex;
}

std::shared_ptr<Texture> Texture::wrap(GLuint id, int width, int height, bool owned) {
    auto tex = std::make_shared<Texture>();
    tex->id_ = id;
    tex->width_ = width;
    tex->height_ = height;
    tex->owned_ = owned;
    return tex;
}

void Texture::update(int x, int y, int width, int height, const void* pixels) {
    bindScratch(id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (format_ == TextureFormat::R8)
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RED, GL_UNSIGNED_BYTE, pixels);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

RenderTarget::~RenderTarget() { release(); }

void RenderTarget::release() {
    if (fbo_) {
        // Deleting the bound framebuffer reverts to 0; draw what is pending into it first.
        if (Renderer::available()) Renderer::instance().flush();
        glDeleteFramebuffers(1, &fbo_);
    }
    fbo_ = 0;
    texture_.reset();
    width_ = height_ = 0;
}

bool RenderTarget::ensure(int pixelWidth, int pixelHeight) {
    if (fbo_ && pixelWidth == width_ && pixelHeight == height_) return true;
    release();
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    texture_ = Texture::create(pixelWidth, pixelHeight, TextureFormat::RGBA8, nullptr, true);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_->id(), 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log::error("gfx", "render target {}x{} incomplete (0x{:x})", pixelWidth, pixelHeight, status);
        release();
        return false;
    }
    width_ = pixelWidth;
    height_ = pixelHeight;
    return true;
}

}  // namespace karafin::gfx
