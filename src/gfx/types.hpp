#pragma once

// Basic value types shared by gfx and ui. All coordinates are logical pixels (1280x720 space)
// unless a name says otherwise.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace karafin::gfx {

struct Vec2 {
    float x = 0, y = 0;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(const Vec2&) const = default;

    float length() const { return std::sqrt(x * x + y * y); }
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}

    // Color::hex(0x7B61FF) or Color::hex(0x7B61FF, 0.5f)
    static constexpr Color hex(uint32_t rgb, float alpha = 1.0f) {
        return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
    }
    static constexpr Color rgba8(int r, int g, int b, int a = 255) {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
    static constexpr Color white(float alpha = 1.0f) { return {1, 1, 1, alpha}; }
    static constexpr Color black(float alpha = 1.0f) { return {0, 0, 0, alpha}; }
    static constexpr Color transparent() { return {0, 0, 0, 0}; }

    constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    constexpr Color mulAlpha(float f) const { return {r, g, b, a * f}; }
    // Mix toward white / black by `amount` (0..1), alpha unchanged.
    constexpr Color lighter(float amount) const {
        return {r + (1 - r) * amount, g + (1 - g) * amount, b + (1 - b) * amount, a};
    }
    constexpr Color darker(float amount) const { return {r * (1 - amount), g * (1 - amount), b * (1 - amount), a}; }
    float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

    static constexpr Color lerp(const Color& x, const Color& y, float t) {
        return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
    }

    // Arithmetic for animation interpolation.
    constexpr Color operator+(const Color& o) const { return {r + o.r, g + o.g, b + o.b, a + o.a}; }
    constexpr Color operator-(const Color& o) const { return {r - o.r, g - o.g, b - o.b, a - o.a}; }
    constexpr Color operator*(float s) const { return {r * s, g * s, b * s, a * s}; }
    constexpr bool operator==(const Color&) const = default;

    // 0xAABBGGRR (byte order R,G,B,A in memory on little endian), clamped.
    uint32_t packed() const {
        auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
    }
};

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;

    static constexpr Insets all(float v) { return {v, v, v, v}; }
    static constexpr Insets symmetric(float horizontal, float vertical) {
        return {horizontal, vertical, horizontal, vertical};
    }
    constexpr float horizontal() const { return left + right; }
    constexpr float vertical() const { return top + bottom; }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    static constexpr Rect fromLTRB(float l, float t, float r, float b) { return {l, t, r - l, b - t}; }

    constexpr float left() const { return x; }
    constexpr float top() const { return y; }
    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr Vec2 origin() const { return {x, y}; }
    constexpr Vec2 size() const { return {w, h}; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr bool empty() const { return w <= 0 || h <= 0; }

    constexpr bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    constexpr bool intersects(const Rect& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    Rect intersection(const Rect& o) const {
        const float l = std::max(x, o.x), t = std::max(y, o.y);
        const float r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    }
    Rect united(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return fromLTRB(std::min(x, o.x), std::min(y, o.y), std::max(right(), o.right()), std::max(bottom(), o.bottom()));
    }
    constexpr Rect inset(const Insets& i) const { return {x + i.left, y + i.top, w - i.horizontal(), h - i.vertical()}; }
    constexpr Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    constexpr Rect expanded(float d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
    constexpr Rect translated(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    constexpr Rect translated(Vec2 d) const { return {x + d.x, y + d.y, w, h}; }
    constexpr bool operator==(const Rect&) const = default;
};

// 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    static constexpr Affine identity() { return {}; }
    static constexpr Affine translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
    static constexpr Affine scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
    // Uniform scale `s` keeping `pivot` fixed.
    static constexpr Affine scaleAround(float s, Vec2 pivot) {
        return {s, 0, 0, s, pivot.x * (1 - s), pivot.y * (1 - s)};
    }
    static Affine rotate(float radians) {
        const float cs = std::cos(radians), sn = std::sin(radians);
        return {cs, sn, -sn, cs, 0, 0};
    }

    // (A * B)(p) == A(B(p))
    constexpr Affine operator*(const Affine& o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }
    constexpr Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    constexpr Vec2 applyVector(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    constexpr bool hasRotation() const { return b != 0 || c != 0; }
    constexpr bool isTranslation() const { return a == 1 && d == 1 && b == 0 && c == 0; }
    // Average linear scale factor (for stroke widths / culling margins).
    float approxScale() const { return std::sqrt(std::fabs(a * d - b * c)); }
    // Axis-aligned bounds of a transformed rect.
    Rect mapRect(const Rect& r) const {
        const Vec2 p0 = apply({r.x, r.y}), p1 = apply({r.right(), r.y});
        const Vec2 p2 = apply({r.x, r.bottom()}), p3 = apply({r.right(), r.bottom()});
        const float l = std::min({p0.x, p1.x, p2.x, p3.x}), t = std::min({p0.y, p1.y, p2.y, p3.y});
        const float rr = std::max({p0.x, p1.x, p2.x, p3.x}), bb = std::max({p0.y, p1.y, p2.y, p3.y});
        return Rect::fromLTRB(l, t, rr, bb);
    }
    Affine inverted() const {
        const float det = a * d - b * c;
        if (std::fabs(det) < 1e-12f) return {};
        const float id = 1.0f / det;
        Affine r{d * id, -b * id, -c * id, a * id, 0, 0};
        r.tx = -(r.a * tx + r.c * ty);
        r.ty = -(r.b * tx + r.d * ty);
        return r;
    }
};

inline constexpr float kPi = 3.14159265358979323846f;

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

enum class ImageFit : uint8_t {
    Cover,    // fill the box, crop overflow (keeps aspect)
    Contain,  // fit inside the box, letterbox (keeps aspect)
    Fill,     // stretch to the box
};

}  // namespace karafin::gfx
