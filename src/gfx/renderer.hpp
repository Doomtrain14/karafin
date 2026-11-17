#pragma once

// Batched GLES3 2D renderer.
//
// Everything is drawn through one uber-shader and one dynamic vertex buffer. Each primitive is a
// quad carrying its own shape parameters (signed-distance rounded rect / border / shadow /
// triangle / arc / glyph / textured), so consecutive primitives batch into a single draw call.
// Up to 8 textures are bound per batch (glyph atlas pages + images), so interleaving text, shapes
// and images does not break batches. Batches flush on clip changes, render target changes, slot
// exhaustion or a full buffer.
//
// Coordinates are logical pixels (1280x720 for the main framebuffer) with y down; the display
// scale (1.0 handheld, 1.5 docked) is applied by the projection. Anti-aliasing is computed per
// fragment from screen-space derivatives, so shapes stay crisp under any transform/scale.
//
// Colors are straight alpha in the API; blending is premultiplied (ONE, ONE_MINUS_SRC_ALPHA).
// Textures must contain premultiplied RGBA (the image loader does this).
//
// External GL (mpv render API) inside a frame:
//   auto ext = renderer.beginExternal();   // flushes; ext.fbo / ext.width / ext.height
//   mpv_render_context_render(...);        // draw into ext.fbo
//   renderer.endExternal();                // == resetState(): restores all GL state we rely on

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <GLES3/gl3.h>

#include "core/app.hpp"
#include "gfx/text.hpp"
#include "gfx/texture.hpp"
#include "gfx/types.hpp"

namespace karafin::gfx {

// Four-corner color fill. Interpolated linearly across the primitive's box.
struct Gradient {
    Color topLeft, topRight, bottomRight, bottomLeft;

    static constexpr Gradient solid(Color c) { return {c, c, c, c}; }
    static constexpr Gradient vertical(Color top, Color bottom) { return {top, top, bottom, bottom}; }
    static constexpr Gradient horizontal(Color left, Color right) { return {left, right, right, left}; }
};

struct ImageStyle {
    ImageFit fit = ImageFit::Cover;
    float radius = 0.0f;           // rounded corners (logical px)
    Color tint = Color::white();   // multiplied; use alpha for fades
    Vec2 focus = {0.5f, 0.5f};     // which part stays visible when Cover crops (0..1)
    Rect uv = {0, 0, 1, 1};        // source sub-rect in normalized texture coordinates
    // Under a rotation the quad's edges are anti-aliased. Off: hard edges, for quads that tile
    // seamlessly along shared edges (the image's own alpha has to soften its outline).
    bool antialias = true;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // The live renderer (set by init, cleared by shutdown).
    static Renderer& instance();
    static bool available();

    // Compiles shaders, creates buffers and initializes text. Needs a current GL context.
    bool init();
    void shutdown();

    // Frame bracket. beginFrame binds the default framebuffer, sets the viewport for `display`,
    // resets all stacks and GL state. endFrame flushes and checks that the stacks are balanced.
    void beginFrame(const DisplayInfo& display);
    void endFrame();

    // Clears the current target (scissor-aware: clears only the current clip region).
    void clear(Color color);
    // Submits pending geometry (one draw call). Needed before foreign GL code draws.
    void flush();
    // Re-applies every bit of GL state the renderer depends on and forgets cached bindings:
    // program, VAO + vertex layout, VBO/IBO, unpack PBO/row length/alignment, blend enable + func
    // + equation, color mask, depth/stencil/cull/polygon-offset/coverage disabled, scissor test +
    // rect for the clip stack, framebuffer + viewport + projection of the current target, sampler
    // objects and the texture-unit cache. Call after any foreign GL code (mpv, custom GL).
    void resetState();
    void restoreState() { resetState(); }

    struct ExternalTarget {
        GLuint fbo = 0;       // framebuffer currently targeted (0 = default)
        int width = 0;        // pixel size of that target
        int height = 0;
        bool flipY = false;   // true when rendering into an offscreen target (y-up texture space)
    };
    // Flushes and unbinds our VAO; returns the current target for the foreign renderer.
    ExternalTarget beginExternal();
    void endExternal() { resetState(); }

    // Called by Texture before it deletes a GL texture: flushes quads that still sample it and
    // drops it from the texture-unit cache (GL ids are recycled).
    void forgetTexture(GLuint id);

    float scale() const { return targetScale_; }
    // Logical size of the current target.
    Vec2 viewportSize() const { return {targetWidth_, targetHeight_}; }
    const DisplayInfo& display() const { return display_; }

    // ---- Shapes ---------------------------------------------------------------------------
    void fillRect(const Rect& r, Color c);
    void fillRect(const Rect& r, const Gradient& g);
    void fillRoundRect(const Rect& r, float radius, Color c);
    void fillRoundRect(const Rect& r, float radius, const Gradient& g);
    // Border drawn inside `r`.
    void strokeRoundRect(const Rect& r, float radius, float width, Color c);
    // Soft gaussian-like shadow of a rounded rect. `blur` ~ standard deviation * 2 in logical px.
    void drawShadow(const Rect& r, float radius, float blur, Color c, Vec2 offset = {}, float spread = 0.0f);
    void fillCircle(Vec2 center, float radius, Color c);
    void strokeCircle(Vec2 center, float radius, float width, Color c);
    // Line segment with round caps.
    void drawLine(Vec2 a, Vec2 b, float width, Color c);
    // Isosceles triangle in a `size` box centered at `center`, apex pointing up for rotation 0
    // (rotation in radians, clockwise). `cornerRadius` rounds the corners.
    void fillTriangle(Vec2 center, Vec2 size, float rotation, Color c, float cornerRadius = 0.0f);
    // Circular arc (stroke). Angles in radians, 0 = +x (3 o'clock), positive = clockwise.
    void drawArc(Vec2 center, float radius, float width, float startAngle, float sweep, Color c);
    // Skeleton placeholder: rounded rect with a moving highlight band. `phase` in [0,1).
    void fillShimmer(const Rect& r, float radius, Color base, float phase, float intensity = 0.07f);

    // ---- Images ---------------------------------------------------------------------------
    void drawTexture(const Texture& tex, const Rect& dst, const Rect& uv = {0, 0, 1, 1},
                     Color tint = Color::white(), float radius = 0.0f, bool antialias = true);
    void drawImage(const Texture& tex, const Rect& dst, const ImageStyle& style = {});
    void drawRenderTarget(const RenderTarget& rt, const Rect& dst, float alpha = 1.0f);

    // ---- Text -----------------------------------------------------------------------------
    void drawText(const TextLayout& layout, Vec2 topLeft, Color c);
    // Convenience: layout (cached) + draw. Returns the layout size.
    Vec2 drawText(std::string_view str, Vec2 topLeft, const TextStyle& style, Color c,
                  const TextOptions& options = {});

    // ---- State stacks ----------------------------------------------------------------------
    // Clip rect in the current transform space (rotation is ignored: the AABB is used).
    void pushClip(const Rect& r);
    void popClip();
    // Clip rect in target-space logical px.
    Rect clipRect() const { return clip_; }

    void pushAlpha(float alpha);  // multiplies
    void popAlpha();
    float alpha() const { return alpha_; }

    void pushTransform(const Affine& t);  // post-multiplies (applies in local space)
    void pushTranslate(float dx, float dy) { pushTransform(Affine::translate(dx, dy)); }
    void pushScale(float s, Vec2 pivot) { pushTransform(Affine::scaleAround(s, pivot)); }
    void popTransform();
    const Affine& transform() const { return transform_; }

    // Redirects drawing into `rt` (sized to the target's pixels / display scale). Stacks are
    // saved and reset for the target; popTarget restores them.
    void pushTarget(RenderTarget& rt, bool clearTarget = true, Color clearColor = Color::transparent());
    void popTarget();

    // True if `localRect` (current transform space) intersects the visible clip area.
    bool isVisible(const Rect& localRect) const;

    struct Stats {
        int drawCalls = 0;
        int quads = 0;
        int targets = 0;  // offscreen passes (pushTarget)
    };
    const Stats& lastFrameStats() const { return lastStats_; }

private:
    struct Vertex {
        float x, y;            // target-space logical position (transformed)
        float u, v;            // texture coords, or shape extras
        float lx, ly;          // shape-local coordinates (relative to shape center)
        float hw, hh;          // shape half extents
        float radius, border;  // corner radius / border width
        float extra, kind;     // kind-specific parameter, kind + 16 * (slot + 1)
        uint32_t color;        // RGBA8
    };

    enum Kind : int { kFlat = 0, kGlyph = 1, kRRect = 2, kShadow = 3, kTriangle = 4, kArc = 5 };

    struct Quad {
        Vec2 center;           // shape center in current transform space
        Vec2 half;             // shape half extents
        float rotation = 0;    // radians around center
        float margin = 1;      // extra quad size around the shape (AA / blur)
        int kind = kFlat;
        const Texture* texture = nullptr;
        Rect uv = {0, 0, 1, 1};    // uv of the un-expanded shape box
        Vec2 extraUv = {0, 0};     // used instead of uv when untextured
        float radius = 0, border = 0, extra = 0;
        Gradient colors;
    };

    void emit(const Quad& q);
    void emitGlyphQuad(float x0, float y0, float x1, float y1, const text::GlyphQuad& g, uint32_t color);
    int slotFor(const Texture* tex);
    void applyScissor();
    void applyTargetViewport();
    void setProjection();
    void setVertexLayout(size_t baseOffset);
    // GL_EXT_disjoint_timer_query around the frame; results feed perf::setGpuMs a few frames later.
    void beginGpuTimer();
    void endGpuTimer(bool discard);

    static constexpr GLenum kTimeElapsed = 0x88BF;  // GL_TIME_ELAPSED_EXT
    static constexpr GLenum kGpuDisjoint = 0x8FBB;  // GL_GPU_DISJOINT_EXT
    bool timerQueries_ = false;
    bool queryActive_ = false;
    std::array<GLuint, 4> queries_{};
    std::array<bool, 4> queryPending_{};
    std::array<bool, 4> queryDiscard_{};
    size_t queryIndex_ = 0;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLint uProj_ = -1;
    size_t vboCapacity_ = 0;
    size_t vboOffset_ = 0;   // append position in the streaming VBO
    size_t layoutBase_ = 0;  // byte offset the vertex attribute pointers currently use

    std::vector<Vertex> vertices_;
    std::array<GLuint, 8> slots_{};  // textures of the current batch, by unit
    int slotCount_ = 0;
    std::array<GLuint, 8> bound_{};  // what each unit actually holds (0xFFFFFFFF = unknown)

    DisplayInfo display_;
    float targetScale_ = 1.0f;
    float targetWidth_ = 1280.0f;
    float targetHeight_ = 720.0f;
    int targetPixelW_ = 1280;
    int targetPixelH_ = 720;
    GLuint targetFbo_ = 0;
    bool targetFlip_ = false;

    Affine transform_;
    float alpha_ = 1.0f;
    Rect clip_;
    std::vector<Affine> transformStack_;
    std::vector<float> alphaStack_;
    std::vector<Rect> clipStack_;

    struct TargetState {
        GLuint fbo;
        int pixelW, pixelH;
        float width, height;
        bool flip;
        Affine transform;
        float alpha;
        Rect clip;
        size_t transformDepth, alphaDepth, clipDepth;
    };
    std::vector<TargetState> targetStack_;

    Stats stats_;
    Stats lastStats_;
    bool inFrame_ = false;
};

}  // namespace karafin::gfx
