#include "gfx/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/perf.hpp"

namespace karafin::gfx {
namespace {

Renderer* g_renderer = nullptr;

constexpr int kMaxQuads = 16384;  // per draw call (uint16 indices)
constexpr int kMaxSlots = 8;
constexpr GLuint kUnknownBinding = 0xFFFFFFFFu;
// Streaming vertex buffer: batches are appended and the storage is orphaned only when it wraps,
// so the driver can write unsynchronized instead of reallocating on every flush.
constexpr size_t kInitialVboBytes = 2u * 1024 * 1024;

const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec2 aLocal;
layout(location = 3) in vec2 aHalf;
layout(location = 4) in vec4 aParams;
layout(location = 5) in vec4 aColor;
uniform vec4 uProj;
out vec2 vUv;
out vec2 vLocal;
flat out vec2 vHalf;
flat out vec4 vParams;
out vec4 vColor;
void main() {
    vUv = aUv;
    vLocal = aLocal;
    vHalf = aHalf;
    vParams = aParams;
    vColor = aColor;
    gl_Position = vec4(aPos * uProj.xy + uProj.zw, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 300 es
precision highp float;
in vec2 vUv;
in vec2 vLocal;
flat in vec2 vHalf;
flat in vec4 vParams;
in vec4 vColor;
uniform sampler2D uTex0;
uniform sampler2D uTex1;
uniform sampler2D uTex2;
uniform sampler2D uTex3;
uniform sampler2D uTex4;
uniform sampler2D uTex5;
uniform sampler2D uTex6;
uniform sampler2D uTex7;
out vec4 oColor;

vec4 sampleSlot(int s, vec2 uv) {
    if (s < 4) {
        if (s < 2) return s == 0 ? texture(uTex0, uv) : texture(uTex1, uv);
        return s == 2 ? texture(uTex2, uv) : texture(uTex3, uv);
    }
    if (s < 6) return s == 4 ? texture(uTex4, uv) : texture(uTex5, uv);
    return s == 6 ? texture(uTex6, uv) : texture(uTex7, uv);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float erfApprox(float x) {
    float s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

// Isosceles triangle, apex at the origin, base at y = q.y (iq).
float sdTriangle(vec2 p, vec2 q) {
    p.x = abs(p.x);
    vec2 a = p - q * clamp(dot(p, q) / dot(q, q), 0.0, 1.0);
    vec2 b = p - q * vec2(clamp(p.x / q.x, 0.0, 1.0), 1.0);
    float s = -sign(q.y);
    vec2 d = min(vec2(dot(a, a), s * (p.x * q.y - p.y * q.x)), vec2(dot(b, b), s * (p.y - q.y)));
    return -sqrt(d.x) * sign(d.y);
}

// Arc symmetric around +y with aperture sin/cos `sc` (iq).
float sdArc(vec2 p, vec2 sc, float ra, float rb) {
    p.x = abs(p.x);
    return ((sc.y * p.x > sc.x * p.y) ? length(p - sc * ra) : abs(length(p) - ra)) - rb;
}

void main() {
    // Derivatives first, in uniform control flow (they are undefined after divergent branches).
    float px = max(length(fwidth(vLocal)) * 0.70710678, 1e-4);
    float k = vParams.w;
    int kind = int(mod(k, 16.0) + 0.5);
    int slot = int(floor(k / 16.0 + 0.01)) - 1;
    vec4 c = vColor;

    if (kind == 1) {
        float a = sampleSlot(slot, vUv).r;
        // Light text on dark backgrounds reads thin with linear coverage; lift it a little.
        float lum = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        a = pow(a, mix(1.0, 0.8, lum));
        oColor = vec4(c.rgb, 1.0) * (c.a * a);
        return;
    }

    float cov = 1.0;
    vec4 texel = vec4(1.0);
    if (slot >= 0) texel = sampleSlot(slot, vUv);

    if (kind == 2) {
        float d = sdRoundBox(vLocal, vHalf, vParams.x);
        cov = clamp(0.5 - d / px, 0.0, 1.0);
        if (vParams.y > 0.0) cov *= clamp(0.5 + (d + vParams.y) / px, 0.0, 1.0);
        if (vParams.z > 0.0 && slot < 0) {
            float band = 1.0 - smoothstep(0.0, vUv.y, abs(vLocal.x + vLocal.y * 0.35 - vUv.x));
            c.rgb += vec3(vParams.z * band);
        }
    } else if (kind == 3) {
        float sigma = max(vParams.z, 0.5);
        float d = sdRoundBox(vLocal, vHalf, vParams.x);
        cov = 0.5 - 0.5 * erfApprox(d / (sigma * 1.41421356));
    } else if (kind == 4) {
        float d = sdTriangle(vec2(vLocal.x, vLocal.y + vHalf.y), vec2(vHalf.x, 2.0 * vHalf.y)) - vParams.x;
        cov = clamp(0.5 - d / px, 0.0, 1.0);
    } else if (kind == 5) {
        float d = sdArc(vLocal, vUv, vParams.x, vParams.y * 0.5);
        cov = clamp(0.5 - d / px, 0.0, 1.0);
    }

    vec4 outColor = texel * vec4(c.rgb * c.a, c.a) * cov;
    // Interleaved gradient noise (Jimenez): hides 8-bit banding in dark gradients and scrims
    // without the transcendental sin() hash, which ran for every pixel of every full-screen layer.
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    outColor.rgb += (n - 0.5) * (1.5 / 255.0) * outColor.a;
    oColor = outColor;
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        GLsizei len = 0;
        glGetShaderInfoLog(sh, sizeof(buf), &len, buf);
        log::error("gfx", "shader compile failed: {}", std::string_view(buf, static_cast<size_t>(len)));
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

Color bilinear(const Gradient& g, float tx, float ty) {
    const Color top = Color::lerp(g.topLeft, g.topRight, tx);
    const Color bottom = Color::lerp(g.bottomLeft, g.bottomRight, tx);
    return Color::lerp(top, bottom, ty);
}

}  // namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (g_renderer == this) shutdown();
}

Renderer& Renderer::instance() { return *g_renderer; }
bool Renderer::available() { return g_renderer != nullptr; }

bool Renderer::init() {
    main_thread::assertCurrent("Renderer::init");
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        GLsizei len = 0;
        glGetProgramInfoLog(program_, sizeof(buf), &len, buf);
        log::error("gfx", "program link failed: {}", std::string_view(buf, static_cast<size_t>(len)));
        return false;
    }
    uProj_ = glGetUniformLocation(program_, "uProj");
    glUseProgram(program_);
    for (int i = 0; i < kMaxSlots; ++i) {
        const std::string name = "uTex" + std::to_string(i);
        glUniform1i(glGetUniformLocation(program_, name.c_str()), i);
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    vboCapacity_ = kInitialVboBytes;
    vboOffset_ = 0;
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vboCapacity_), nullptr, GL_STREAM_DRAW);

    std::vector<uint16_t> indices(kMaxQuads * 6);
    for (int i = 0; i < kMaxQuads; ++i) {
        const auto b = static_cast<uint16_t>(i * 4);
        uint16_t* idx = &indices[static_cast<size_t>(i) * 6];
        idx[0] = b;
        idx[1] = static_cast<uint16_t>(b + 1);
        idx[2] = static_cast<uint16_t>(b + 2);
        idx[3] = b;
        idx[4] = static_cast<uint16_t>(b + 2);
        idx[5] = static_cast<uint16_t>(b + 3);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint16_t)), indices.data(),
                 GL_STATIC_DRAW);

    for (GLuint loc = 0; loc <= 5; ++loc) glEnableVertexAttribArray(loc);
    setVertexLayout(0);
    glBindVertexArray(0);

    vertices_.reserve(4 * 4096);
    bound_.fill(kUnknownBinding);
    g_renderer = this;

    GLint extCount = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extCount);
    for (GLint i = 0; i < extCount; ++i) {
        const auto* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (ext && std::string_view(ext) == "GL_EXT_disjoint_timer_query") timerQueries_ = true;
    }
    if (timerQueries_) {
        glGenQueries(static_cast<GLsizei>(queries_.size()), queries_.data());
        while (glGetError() != GL_NO_ERROR) {
        }
    }
    log::info("gfx", "GPU timer queries {}", timerQueries_ ? "available" : "unavailable");

    if (!text::init()) log::error("gfx", "text init failed; text will not render");
    log::info("gfx", "renderer ready");
    return true;
}

void Renderer::setVertexLayout(size_t base) {
    const auto stride = static_cast<GLsizei>(sizeof(Vertex));
    auto attr = [&](GLuint loc, GLint n, size_t offset) {
        glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(base + offset));
    };
    attr(0, 2, offsetof(Vertex, x));
    attr(1, 2, offsetof(Vertex, u));
    attr(2, 2, offsetof(Vertex, lx));
    attr(3, 2, offsetof(Vertex, hw));
    attr(4, 4, offsetof(Vertex, radius));
    glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<const void*>(base + offsetof(Vertex, color)));
    layoutBase_ = base;
}

void Renderer::shutdown() {
    if (g_renderer != this) return;
    text::shutdown();
    if (timerQueries_) {
        if (queryActive_) glEndQuery(kTimeElapsed);
        glDeleteQueries(static_cast<GLsizei>(queries_.size()), queries_.data());
        queries_.fill(0);
        timerQueries_ = queryActive_ = false;
    }
    if (ibo_) glDeleteBuffers(1, &ibo_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    ibo_ = vbo_ = vao_ = program_ = 0;
    g_renderer = nullptr;
}

void Renderer::beginFrame(const DisplayInfo& display) {
    display_ = display;
    inFrame_ = true;
    stats_ = {};
    targetScale_ = display.scale > 0 ? display.scale : 1.0f;
    targetPixelW_ = display.pixelWidth;
    targetPixelH_ = display.pixelHeight;
    targetWidth_ = static_cast<float>(display.pixelWidth) / targetScale_;
    targetHeight_ = static_cast<float>(display.pixelHeight) / targetScale_;
    targetFbo_ = 0;
    targetFlip_ = false;
    transform_ = {};
    alpha_ = 1.0f;
    clip_ = {0, 0, targetWidth_, targetHeight_};
    transformStack_.clear();
    alphaStack_.clear();
    clipStack_.clear();
    targetStack_.clear();
    text::setDisplayScale(targetScale_);
    resetState();
    beginGpuTimer();
}

void Renderer::endFrame() {
    flush();
    if (!transformStack_.empty() || !alphaStack_.empty() || !clipStack_.empty() || !targetStack_.empty())
        log::warn("gfx", "unbalanced stacks at endFrame (t{} a{} c{} rt{})", transformStack_.size(), alphaStack_.size(),
                  clipStack_.size(), targetStack_.size());
    while (!targetStack_.empty()) popTarget();
    endGpuTimer(false);
    lastStats_ = stats_;
    perf::Frame& pf = perf::frame();
    pf.drawCalls += stats_.drawCalls;
    pf.quads += stats_.quads;
    pf.targets += stats_.targets;
    inFrame_ = false;
    text::endFrame();
}

void Renderer::beginGpuTimer() {
    if (!timerQueries_) return;
    // Collect finished queries (a few frames old) without stalling on unfinished ones.
    GLint disjoint = 0;
    glGetIntegerv(kGpuDisjoint, &disjoint);
    for (size_t i = 0; i < queries_.size(); ++i) {
        if (!queryPending_[i]) continue;
        GLuint available = 0;
        glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) continue;
        GLuint ns = 0;
        glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT, &ns);
        queryPending_[i] = false;
        if (!disjoint && !queryDiscard_[i]) perf::setGpuMs(static_cast<double>(ns) / 1e6);
    }
    if (queryPending_[queryIndex_]) return;  // GPU is more than a ring behind; skip this frame
    glBeginQuery(kTimeElapsed, queries_[queryIndex_]);
    queryActive_ = true;
}

void Renderer::endGpuTimer(bool discard) {
    if (!queryActive_) return;
    glEndQuery(kTimeElapsed);
    queryActive_ = false;
    queryPending_[queryIndex_] = true;
    queryDiscard_[queryIndex_] = discard;
    queryIndex_ = (queryIndex_ + 1) % queries_.size();
}

void Renderer::setProjection() {
    if (targetFlip_)
        glUniform4f(uProj_, 2.0f / targetWidth_, 2.0f / targetHeight_, -1.0f, -1.0f);
    else
        glUniform4f(uProj_, 2.0f / targetWidth_, -2.0f / targetHeight_, -1.0f, 1.0f);
}

void Renderer::applyTargetViewport() {
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo_);
    glViewport(0, 0, targetPixelW_, targetPixelH_);
    setProjection();
    applyScissor();
}

void Renderer::resetState() {
    flush();
    glUseProgram(program_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    // Pixel transfer state foreign code (mpv uploads) may leave behind; a bound unpack PBO would
    // turn our glTex(Sub)Image2D pointers into buffer offsets.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_COVERAGE);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_DITHER);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < kMaxSlots; ++i) glBindSampler(static_cast<GLuint>(i), 0);
    // Texture units may hold anything now: rebind on next use.
    slotCount_ = 0;
    slots_.fill(0);
    bound_.fill(kUnknownBinding);
    // Re-specify the vertex layout (cheap; guards against foreign code editing our VAO).
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    setVertexLayout(layoutBase_);
    applyTargetViewport();
}

Renderer::ExternalTarget Renderer::beginExternal() {
    flush();
    // Time-elapsed queries cannot nest and mpv times its own passes: stop ours for this frame.
    endGpuTimer(true);
    // Foreign code then only edits the default VAO, never ours.
    glBindVertexArray(0);
    return {targetFbo_, targetPixelW_, targetPixelH_, targetFlip_};
}

void Renderer::forgetTexture(GLuint id) {
    if (id == 0) return;
    for (int i = 0; i < slotCount_; ++i)
        if (slots_[static_cast<size_t>(i)] == id) {
            flush();  // pending quads sample it: draw them before it goes away
            break;
        }
    // glDeleteTextures resets these units to 0; a recycled id must not look bound.
    for (GLuint& b : bound_)
        if (b == id) b = 0;
}

void Renderer::clear(Color c) {
    flush();
    glClearColor(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::flush() {
    if (vertices_.empty()) return;
    const size_t bytes = vertices_.size() * sizeof(Vertex);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (bytes > vboCapacity_) {
        vboCapacity_ = std::max(bytes, vboCapacity_ * 2);
        vboOffset_ = vboCapacity_;  // forces the orphan below
    }
    if (vboOffset_ + bytes > vboCapacity_) {
        // Wrapped: orphan so in-flight draws keep the old storage.
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vboCapacity_), nullptr, GL_STREAM_DRAW);
        vboOffset_ = 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(vboOffset_), static_cast<GLsizeiptr>(bytes),
                    vertices_.data());
    // Glyphs rasterized since the last draw land in the atlas as one sub-image per page.
    text::flushAtlas();
    if (layoutBase_ != vboOffset_) setVertexLayout(vboOffset_);
    vboOffset_ += bytes;
    const auto quads = static_cast<GLsizei>(vertices_.size() / 4);
    glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, nullptr);
    stats_.drawCalls++;
    stats_.quads += quads;
    vertices_.clear();
    slotCount_ = 0;
}

int Renderer::slotFor(const Texture* tex) {
    if (!tex) return -1;
    const GLuint id = tex->id();
    for (int i = 0; i < slotCount_; ++i)
        if (slots_[static_cast<size_t>(i)] == id) return i;
    if (slotCount_ == kMaxSlots) flush();
    const int slot = slotCount_++;
    slots_[static_cast<size_t>(slot)] = id;
    // Batches tend to use textures in the same order (glyph atlas first), so units often still
    // hold the right texture from the previous batch.
    if (bound_[static_cast<size_t>(slot)] != id) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(slot));
        glBindTexture(GL_TEXTURE_2D, id);
        bound_[static_cast<size_t>(slot)] = id;
    }
    return slot;
}

void Renderer::applyScissor() {
    const float s = targetScale_;
    const int x0 = static_cast<int>(std::lround(clip_.x * s));
    const int y0 = static_cast<int>(std::lround(clip_.y * s));
    const int x1 = static_cast<int>(std::lround(clip_.right() * s));
    const int y1 = static_cast<int>(std::lround(clip_.bottom() * s));
    const int w = std::max(0, x1 - x0), h = std::max(0, y1 - y0);
    if (targetFlip_)
        glScissor(x0, y0, w, h);
    else
        glScissor(x0, targetPixelH_ - y1, w, h);
}

void Renderer::emit(const Quad& q) {
    if (alpha_ <= 0.0f) return;
    if (q.colors.topLeft.a <= 0.0f && q.colors.topRight.a <= 0.0f && q.colors.bottomRight.a <= 0.0f &&
        q.colors.bottomLeft.a <= 0.0f)
        return;  // premultiplied: fully transparent contributes nothing
    const float hw = q.half.x, hh = q.half.y;
    const float ex = hw + q.margin, ey = hh + q.margin;
    const Vec2 local[4] = {{-ex, -ey}, {ex, -ey}, {ex, ey}, {-ex, ey}};

    Affine m = transform_ * Affine::translate(q.center.x, q.center.y);
    if (q.rotation != 0.0f) m = m * Affine::rotate(q.rotation);
    Vec2 pts[4];
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; ++i) {
        pts[i] = m.apply(local[i]);
        x0 = std::min(x0, pts[i].x);
        y0 = std::min(y0, pts[i].y);
        x1 = std::max(x1, pts[i].x);
        y1 = std::max(y1, pts[i].y);
    }
    // Entirely outside the clip: scissored away anyway, skip the vertices (and any batch break
    // its texture would cause).
    if (x1 <= clip_.x || y1 <= clip_.y || x0 >= clip_.right() || y0 >= clip_.bottom()) return;

    if (vertices_.size() + 4 > static_cast<size_t>(kMaxQuads) * 4) flush();
    const int slot = slotFor(q.texture);
    const float kindCode = static_cast<float>(q.kind + 16 * (slot + 1));

    const Color* cs[4] = {&q.colors.topLeft, &q.colors.topRight, &q.colors.bottomRight, &q.colors.bottomLeft};
    const bool uniformColor = q.colors.topLeft == q.colors.topRight && q.colors.topLeft == q.colors.bottomRight &&
                              q.colors.topLeft == q.colors.bottomLeft;

    for (int i = 0; i < 4; ++i) {
        const Vec2 p = pts[i];
        Vertex v;
        v.x = p.x;
        v.y = p.y;
        if (q.texture) {
            const float tx = hw > 0 ? (local[i].x + hw) / (2 * hw) : 0.0f;
            const float ty = hh > 0 ? (local[i].y + hh) / (2 * hh) : 0.0f;
            v.u = q.uv.x + tx * q.uv.w;
            v.v = q.uv.y + ty * q.uv.h;
        } else {
            v.u = q.extraUv.x;
            v.v = q.extraUv.y;
        }
        v.lx = local[i].x;
        v.ly = local[i].y;
        v.hw = hw;
        v.hh = hh;
        v.radius = q.radius;
        v.border = q.border;
        v.extra = q.extra;
        v.kind = kindCode;
        Color c;
        if (uniformColor) {
            c = *cs[0];
        } else {
            const float tx = hw > 0 ? std::clamp((local[i].x + hw) / (2 * hw), 0.0f, 1.0f) : 0.0f;
            const float ty = hh > 0 ? std::clamp((local[i].y + hh) / (2 * hh), 0.0f, 1.0f) : 0.0f;
            c = bilinear(q.colors, tx, ty);
        }
        c.a *= alpha_;
        v.color = c.packed();
        vertices_.push_back(v);
    }
}

namespace {
float aaMargin(const Affine& t) {
    const float s = t.approxScale();
    return s > 0.01f ? std::max(1.0f, 1.0f / s) : 1.0f;
}
}  // namespace

void Renderer::fillRect(const Rect& r, Color c) { fillRect(r, Gradient::solid(c)); }

void Renderer::fillRect(const Rect& r, const Gradient& g) {
    if (r.empty()) return;
    Quad q;
    q.center = r.center();
    q.half = {r.w * 0.5f, r.h * 0.5f};
    q.margin = 0;
    q.kind = kFlat;
    q.colors = g;
    emit(q);
}

void Renderer::fillRoundRect(const Rect& r, float radius, Color c) { fillRoundRect(r, radius, Gradient::solid(c)); }

void Renderer::fillRoundRect(const Rect& r, float radius, const Gradient& g) {
    if (r.empty()) return;
    Quad q;
    q.center = r.center();
    q.half = {r.w * 0.5f, r.h * 0.5f};
    q.margin = aaMargin(transform_);
    q.kind = kRRect;
    q.radius = std::clamp(radius, 0.0f, std::min(q.half.x, q.half.y));
    q.colors = g;
    emit(q);
}

void Renderer::strokeRoundRect(const Rect& r, float radius, float width, Color c) {
    if (r.empty() || width <= 0) return;
    Quad q;
    q.center = r.center();
    q.half = {r.w * 0.5f, r.h * 0.5f};
    q.margin = aaMargin(transform_);
    q.kind = kRRect;
    q.radius = std::clamp(radius, 0.0f, std::min(q.half.x, q.half.y));
    q.border = width;
    q.colors = Gradient::solid(c);
    emit(q);
}

void Renderer::drawShadow(const Rect& r, float radius, float blur, Color c, Vec2 offset, float spread) {
    if (r.empty() || c.a <= 0) return;
    const float sigma = std::max(0.5f, blur * 0.5f);
    Quad q;
    q.center = r.center() + offset;
    q.half = {r.w * 0.5f + spread, r.h * 0.5f + spread};
    q.margin = sigma * 3.0f + 1.0f;
    q.kind = kShadow;
    q.radius = std::clamp(radius + spread, 0.0f, std::min(q.half.x, q.half.y));
    q.extra = sigma;
    q.colors = Gradient::solid(c);
    emit(q);
}

void Renderer::fillCircle(Vec2 center, float radius, Color c) {
    fillRoundRect({center.x - radius, center.y - radius, radius * 2, radius * 2}, radius, c);
}

void Renderer::strokeCircle(Vec2 center, float radius, float width, Color c) {
    strokeRoundRect({center.x - radius, center.y - radius, radius * 2, radius * 2}, radius, width, c);
}

void Renderer::drawLine(Vec2 a, Vec2 b, float width, Color c) {
    const Vec2 d = b - a;
    const float len = d.length();
    if (width <= 0) return;
    Quad q;
    q.center = (a + b) * 0.5f;
    q.half = {len * 0.5f + width * 0.5f, width * 0.5f};
    q.rotation = len > 0 ? std::atan2(d.y, d.x) : 0.0f;
    q.margin = aaMargin(transform_);
    q.kind = kRRect;
    q.radius = width * 0.5f;
    q.colors = Gradient::solid(c);
    emit(q);
}

void Renderer::fillTriangle(Vec2 center, Vec2 size, float rotation, Color c, float cornerRadius) {
    const float r = std::clamp(cornerRadius, 0.0f, std::min(size.x, size.y) * 0.25f);
    Quad q;
    q.center = center;
    q.half = {std::max(0.5f, size.x * 0.5f - r), std::max(0.5f, size.y * 0.5f - r)};
    q.rotation = rotation;
    q.margin = aaMargin(transform_) + r;
    q.kind = kTriangle;
    q.radius = r;
    q.colors = Gradient::solid(c);
    emit(q);
}

void Renderer::drawArc(Vec2 center, float radius, float width, float startAngle, float sweep, Color c) {
    if (std::fabs(sweep) >= 2 * kPi - 1e-3f) {
        strokeCircle(center, radius + width * 0.5f, width, c);
        return;
    }
    if (sweep < 0) {
        startAngle += sweep;
        sweep = -sweep;
    }
    const float half = sweep * 0.5f;
    const float mid = startAngle + half;
    const float ext = radius + width * 0.5f;
    Quad q;
    q.center = center;
    q.half = {ext, ext};
    q.rotation = mid - kPi * 0.5f;
    q.margin = aaMargin(transform_);
    q.kind = kArc;
    q.radius = radius;
    q.border = width;
    q.extraUv = {std::sin(half), std::cos(half)};
    q.colors = Gradient::solid(c);
    emit(q);
}

void Renderer::fillShimmer(const Rect& r, float radius, Color base, float phase, float intensity) {
    if (r.empty()) return;
    Quad q;
    q.center = r.center();
    q.half = {r.w * 0.5f, r.h * 0.5f};
    q.margin = aaMargin(transform_);
    q.kind = kRRect;
    q.radius = std::clamp(radius, 0.0f, std::min(q.half.x, q.half.y));
    q.extra = intensity;
    // Band travels from beyond the left edge to beyond the right edge (local coords).
    const float bandHalf = std::max(40.0f, r.w * 0.35f);
    const float travel = r.w + bandHalf * 2 + r.h * 0.35f;
    q.extraUv = {-q.half.x - bandHalf - r.h * 0.35f * 0.5f + travel * phase, bandHalf};
    q.colors = Gradient::solid(base);
    emit(q);
}

void Renderer::drawTexture(const Texture& tex, const Rect& dst, const Rect& uv, Color tint, float radius) {
    if (dst.empty()) return;
    Quad q;
    q.center = dst.center();
    q.half = {dst.w * 0.5f, dst.h * 0.5f};
    q.texture = &tex;
    q.uv = uv;
    q.colors = Gradient::solid(tint);
    if (radius > 0.0f || transform_.hasRotation()) {
        q.kind = kRRect;
        q.margin = aaMargin(transform_);
        q.radius = std::clamp(radius, 0.0f, std::min(q.half.x, q.half.y));
    } else {
        q.kind = kFlat;
        q.margin = 0;
    }
    emit(q);
}

void Renderer::drawImage(const Texture& tex, const Rect& dst, const ImageStyle& style) {
    if (dst.empty() || tex.width() <= 0 || tex.height() <= 0) return;
    Rect uv = style.uv;
    Rect box = dst;
    const float texAspect = (tex.width() * uv.w) / std::max(1e-6f, tex.height() * uv.h);
    const float boxAspect = dst.w / std::max(1e-6f, dst.h);
    if (style.fit == ImageFit::Cover) {
        if (texAspect > boxAspect) {
            const float frac = boxAspect / texAspect;
            uv.x += (uv.w - uv.w * frac) * std::clamp(style.focus.x, 0.0f, 1.0f);
            uv.w *= frac;
        } else if (texAspect < boxAspect) {
            const float frac = texAspect / boxAspect;
            uv.y += (uv.h - uv.h * frac) * std::clamp(style.focus.y, 0.0f, 1.0f);
            uv.h *= frac;
        }
    } else if (style.fit == ImageFit::Contain) {
        if (texAspect > boxAspect) {
            const float h = dst.w / texAspect;
            box = {dst.x, dst.y + (dst.h - h) * 0.5f, dst.w, h};
        } else {
            const float w = dst.h * texAspect;
            box = {dst.x + (dst.w - w) * 0.5f, dst.y, w, dst.h};
        }
    }
    drawTexture(tex, box, uv, style.tint, style.radius);
}

void Renderer::drawRenderTarget(const RenderTarget& rt, const Rect& dst, float alpha) {
    if (!rt.valid()) return;
    drawTexture(*rt.texture(), dst, {0, 0, 1, 1}, Color::white(alpha), 0.0f);
}

void Renderer::emitGlyphQuad(float x0, float y0, float x1, float y1, const text::GlyphQuad& g, uint32_t color) {
    if (vertices_.size() + 4 > static_cast<size_t>(kMaxQuads) * 4) flush();
    const int slot = slotFor(g.texture);
    const float kindCode = static_cast<float>(kGlyph + 16 * (slot + 1));
    const Vec2 pos[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const Vec2 uvs[4] = {{g.u0, g.v0}, {g.u1, g.v0}, {g.u1, g.v1}, {g.u0, g.v1}};
    for (int i = 0; i < 4; ++i) {
        const Vec2 p = transform_.apply(pos[i]);
        Vertex v{};
        v.x = p.x;
        v.y = p.y;
        v.u = uvs[i].x;
        v.v = uvs[i].y;
        v.kind = kindCode;
        v.color = color;
        vertices_.push_back(v);
    }
}

void Renderer::drawText(const TextLayout& layout, Vec2 topLeft, Color c) {
    if (layout.glyphs.empty() || alpha_ <= 0.0f || c.a <= 0.0f) return;
    const float s = layout.scale > 0 ? layout.scale : targetScale_;
    const float inv = 1.0f / s;
    Color cc = c;
    cc.a *= alpha_;
    const uint32_t color = cc.packed();

    // Cull whole layout early.
    if (!isVisible({topLeft.x - 4, topLeft.y - 4, layout.width + 8, layout.height + 8})) return;

    if (transform_.isTranslation()) {
        // Pixel-snap the origin so glyph bitmaps land exactly on framebuffer pixels.
        const float ox = std::round((topLeft.x + transform_.tx) * s);
        const float oy = std::round((topLeft.y + transform_.ty) * s);
        for (const PlacedGlyph& pg : layout.glyphs) {
            const float penX = ox + pg.x;
            const float fl = std::floor(penX);
            int sub = static_cast<int>((penX - fl) * text::kSubpixelSteps);
            sub = std::clamp(sub, 0, text::kSubpixelSteps - 1);
            const text::GlyphQuad& g = text::glyph(pg.face, pg.glyph, layout.pixelSize, sub);
            if (!g.texture || g.width == 0) continue;
            const float gx = fl + static_cast<float>(g.left);
            const float gy = std::round(oy + pg.y) - static_cast<float>(g.top);
            const float x0 = gx * inv - transform_.tx, y0 = gy * inv - transform_.ty;
            emitGlyphQuad(x0, y0, x0 + g.width * inv, y0 + g.height * inv, g, color);
        }
    } else {
        for (const PlacedGlyph& pg : layout.glyphs) {
            const text::GlyphQuad& g = text::glyph(pg.face, pg.glyph, layout.pixelSize, 0);
            if (!g.texture || g.width == 0) continue;
            const float x0 = topLeft.x + (std::floor(pg.x) + g.left) * inv;
            const float y0 = topLeft.y + (pg.y - g.top) * inv;
            emitGlyphQuad(x0, y0, x0 + g.width * inv, y0 + g.height * inv, g, color);
        }
    }
}

Vec2 Renderer::drawText(std::string_view str, Vec2 topLeft, const TextStyle& style, Color c,
                        const TextOptions& options) {
    auto layout = text::layout(str, style, options);
    if (!layout) return {};
    drawText(*layout, topLeft, c);
    return {layout->width, layout->height};
}

void Renderer::pushClip(const Rect& r) {
    clipStack_.push_back(clip_);
    const Rect next = clip_.intersection(transform_.mapRect(r));
    if (next == clip_) return;
    flush();
    clip_ = next;
    applyScissor();
}

void Renderer::popClip() {
    if (clipStack_.empty()) return;
    const Rect prev = clipStack_.back();
    clipStack_.pop_back();
    if (prev == clip_) return;
    flush();
    clip_ = prev;
    applyScissor();
}

void Renderer::pushAlpha(float a) {
    alphaStack_.push_back(alpha_);
    alpha_ *= std::clamp(a, 0.0f, 1.0f);
}

void Renderer::popAlpha() {
    if (alphaStack_.empty()) return;
    alpha_ = alphaStack_.back();
    alphaStack_.pop_back();
}

void Renderer::pushTransform(const Affine& t) {
    transformStack_.push_back(transform_);
    transform_ = transform_ * t;
}

void Renderer::popTransform() {
    if (transformStack_.empty()) return;
    transform_ = transformStack_.back();
    transformStack_.pop_back();
}

void Renderer::pushTarget(RenderTarget& rt, bool clearTarget, Color clearColor) {
    flush();
    // A texture attached to the draw framebuffer must not stay bound to a unit our program's
    // samplers reference (ES 3.0 4.4.3 rendering feedback loop -> undefined results).
    if (const TexturePtr& t = rt.texture()) {
        for (int i = 0; i < kMaxSlots; ++i) {
            if (bound_[static_cast<size_t>(i)] == t->id() || bound_[static_cast<size_t>(i)] == kUnknownBinding) {
                glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
                glBindTexture(GL_TEXTURE_2D, 0);
                bound_[static_cast<size_t>(i)] = 0;
            }
        }
    }
    targetStack_.push_back({targetFbo_, targetPixelW_, targetPixelH_, targetWidth_, targetHeight_, targetFlip_,
                            transform_, alpha_, clip_, transformStack_.size(), alphaStack_.size(), clipStack_.size()});
    stats_.targets++;
    targetFbo_ = rt.fbo();
    targetPixelW_ = rt.width();
    targetPixelH_ = rt.height();
    targetWidth_ = static_cast<float>(rt.width()) / targetScale_;
    targetHeight_ = static_cast<float>(rt.height()) / targetScale_;
    targetFlip_ = true;
    transform_ = {};
    alpha_ = 1.0f;
    clip_ = {0, 0, targetWidth_, targetHeight_};
    slotCount_ = 0;
    applyTargetViewport();
    if (clearTarget) clear(clearColor);
}

void Renderer::popTarget() {
    if (targetStack_.empty()) return;
    flush();
    const TargetState st = targetStack_.back();
    targetStack_.pop_back();
    targetFbo_ = st.fbo;
    targetPixelW_ = st.pixelW;
    targetPixelH_ = st.pixelH;
    targetWidth_ = st.width;
    targetHeight_ = st.height;
    targetFlip_ = st.flip;
    transform_ = st.transform;
    alpha_ = st.alpha;
    clip_ = st.clip;
    transformStack_.resize(std::min(transformStack_.size(), st.transformDepth));
    alphaStack_.resize(std::min(alphaStack_.size(), st.alphaDepth));
    clipStack_.resize(std::min(clipStack_.size(), st.clipDepth));
    applyTargetViewport();
}

bool Renderer::isVisible(const Rect& localRect) const {
    return transform_.mapRect(localRect).intersects(clip_);
}

}  // namespace karafin::gfx
