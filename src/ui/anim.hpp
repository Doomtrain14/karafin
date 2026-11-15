#pragma once

// Time-based animation primitives.
//
//   Animated<float> scale{1.0f};
//   scale.springTo(1.08f, theme::motion::focus);       // interruptible, keeps velocity
//   scale.tweenTo(0.0f, 0.25f, ease::outCubic);        // fixed duration
//   scale.update(dt);  float v = scale.value();
//
// Springs are solved analytically per step (exact for any dt, never explode), so they stay
// smooth even when a frame takes longer. Retargeting a running spring keeps its velocity, which
// is what makes rapid D-pad focus changes feel fluid.
//
// Timeline runs fire-and-forget animations / delays with callbacks (owned by ui::Root):
//   ui::timeline().animate(0.3f, ease::outCubic, [=](float t) {...}, [=] {...}, this);

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "gfx/types.hpp"
#include "ui/theme.hpp"

namespace karafin::ui {

using gfx::Color;
using gfx::Vec2;

using EasingFn = float (*)(float);

namespace ease {
float linear(float t);
float inQuad(float t);
float outQuad(float t);
float inOutQuad(float t);
float inCubic(float t);
float outCubic(float t);
float inOutCubic(float t);
float outQuart(float t);
float outQuint(float t);
float inOutQuint(float t);
float outExpo(float t);
float inOutSine(float t);
float outBack(float t);
// CSS-style cubic-bezier(x1, y1, x2, y2) evaluated at t.
float cubicBezier(float x1, float y1, float x2, float y2, float t);
// Material "emphasized decelerate" curve (0.05, 0.7, 0.1, 1.0).
float emphasized(float t);
}  // namespace ease

struct SpringSpec {
    float response = 0.38f;  // seconds; roughly the period of one oscillation
    float damping = 1.0f;    // damping ratio: 1 = critically damped, < 1 overshoots
    constexpr SpringSpec() = default;
    constexpr SpringSpec(float r, float d) : response(r), damping(d) {}
    constexpr SpringSpec(theme::motion::SpringPreset p) : response(p.response), damping(p.damping) {}
};

// Advances a damped spring by dt (x = displacement from target). Exact solution.
void stepSpring(float& x, float& v, float dt, const SpringSpec& spec);

// Per-type component access for Animated<T>.
template <class T>
struct AnimTraits;
template <>
struct AnimTraits<float> {
    static constexpr int N = 1;
    static float get(const float& v, int) { return v; }
    static void set(float& v, int, float x) { v = x; }
};
template <>
struct AnimTraits<Vec2> {
    static constexpr int N = 2;
    static float get(const Vec2& v, int i) { return i == 0 ? v.x : v.y; }
    static void set(Vec2& v, int i, float x) { (i == 0 ? v.x : v.y) = x; }
};
template <>
struct AnimTraits<Color> {
    static constexpr int N = 4;
    static float get(const Color& c, int i) { return i == 0 ? c.r : i == 1 ? c.g : i == 2 ? c.b : c.a; }
    static void set(Color& c, int i, float x) { (i == 0 ? c.r : i == 1 ? c.g : i == 2 ? c.b : c.a) = x; }
};

template <class T>
class Animated {
public:
    using Traits = AnimTraits<T>;

    Animated() = default;
    Animated(const T& v) : value_(v), target_(v) {}  // NOLINT(google-explicit-constructor)

    const T& value() const { return value_; }
    const T& target() const { return target_; }
    operator const T&() const { return value_; }  // NOLINT(google-explicit-constructor)
    bool animating() const { return mode_ != Mode::Idle; }

    // Jump without animation.
    void set(const T& v) {
        value_ = target_ = v;
        mode_ = Mode::Idle;
        for (float& x : velocity_) x = 0;
    }

    void springTo(const T& target, SpringSpec spec = {}) {
        target_ = target;
        spec_ = spec;
        mode_ = Mode::Spring;  // velocity carries over (also from a running tween)
    }

    void tweenTo(const T& target, float duration, EasingFn easing = ease::outCubic) {
        if (duration <= 0) {
            set(target);
            return;
        }
        from_ = value_;
        target_ = target;
        duration_ = duration;
        elapsed_ = 0;
        easing_ = easing;
        mode_ = Mode::Tween;
    }

    // Adds an impulse (units/s) to the spring velocity (e.g. touch fling into a spring).
    void setVelocity(const T& v) {
        for (int i = 0; i < Traits::N; ++i) velocity_[i] = Traits::get(v, i);
    }
    T velocity() const {
        T v = value_;
        for (int i = 0; i < Traits::N; ++i) Traits::set(v, i, velocity_[i]);
        return v;
    }

    // Returns true while still animating.
    bool update(float dt) {
        if (mode_ == Mode::Idle || dt <= 0) return mode_ != Mode::Idle;
        if (mode_ == Mode::Tween) {
            elapsed_ += dt;
            const float t = elapsed_ >= duration_ ? 1.0f : elapsed_ / duration_;
            const float e = easing_ ? easing_(t) : t;
            for (int i = 0; i < Traits::N; ++i) {
                const float a = Traits::get(from_, i), b = Traits::get(target_, i);
                const float prev = Traits::get(value_, i);
                const float next = a + (b - a) * e;
                velocity_[i] = (next - prev) / dt;
                Traits::set(value_, i, next);
            }
            if (t >= 1.0f) {
                value_ = target_;
                for (float& x : velocity_) x = 0;
                mode_ = Mode::Idle;
            }
            return mode_ != Mode::Idle;
        }
        bool settled = true;
        for (int i = 0; i < Traits::N; ++i) {
            const float tgt = Traits::get(target_, i);
            float x = Traits::get(value_, i) - tgt;
            float v = velocity_[i];
            stepSpring(x, v, dt, spec_);
            const float scale = std::fmax(1.0f, std::fabs(tgt));
            if (std::fabs(x) > 0.0005f * scale || std::fabs(v) > 0.005f * scale) settled = false;
            Traits::set(value_, i, tgt + x);
            velocity_[i] = v;
        }
        if (settled) {
            value_ = target_;
            for (float& x : velocity_) x = 0;
            mode_ = Mode::Idle;
        }
        return mode_ != Mode::Idle;
    }

private:
    enum class Mode : uint8_t { Idle, Spring, Tween };
    T value_{};
    T target_{};
    T from_{};
    float velocity_[Traits::N] = {};
    SpringSpec spec_;
    float duration_ = 0, elapsed_ = 0;
    EasingFn easing_ = nullptr;
    Mode mode_ = Mode::Idle;
};

// Fire-and-forget animations and delayed calls, ticked once per frame by ui::Root.
class Timeline {
public:
    using Handle = uint64_t;

    // `owner` alive-token: the animation is dropped silently once the owner dies (see View::lifetime()).
    Handle animate(float duration, EasingFn easing, std::function<void(float)> onUpdate,
                   std::function<void()> onDone = {}, std::weak_ptr<void> owner = {});
    Handle after(float delay, std::function<void()> fn, std::weak_ptr<void> owner = {});
    void cancel(Handle h);
    bool active(Handle h) const;

    void update(float dt);
    void clear();

private:
    struct Item {
        Handle id;
        float duration, elapsed;
        EasingFn easing;
        std::function<void(float)> onUpdate;
        std::function<void()> onDone;
        std::weak_ptr<void> owner;
        bool hasOwner;
        bool cancelled;
    };
    std::vector<Item> items_;
    std::vector<Item> adding_;
    Handle next_ = 1;
    bool updating_ = false;
};

}  // namespace karafin::ui
