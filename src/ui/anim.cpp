#include "ui/anim.hpp"

#include <algorithm>

namespace karafin::ui {

namespace ease {
float linear(float t) { return t; }
float inQuad(float t) { return t * t; }
float outQuad(float t) { return 1 - (1 - t) * (1 - t); }
float inOutQuad(float t) { return t < 0.5f ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2.0f) / 2; }
float inCubic(float t) { return t * t * t; }
float outCubic(float t) { return 1 - std::pow(1 - t, 3.0f); }
float inOutCubic(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2; }
float outQuart(float t) { return 1 - std::pow(1 - t, 4.0f); }
float outQuint(float t) { return 1 - std::pow(1 - t, 5.0f); }
float inOutQuint(float t) { return t < 0.5f ? 16 * t * t * t * t * t : 1 - std::pow(-2 * t + 2, 5.0f) / 2; }
float outExpo(float t) { return t >= 1 ? 1 : 1 - std::pow(2.0f, -10 * t); }
float inOutSine(float t) { return -(std::cos(gfx::kPi * t) - 1) / 2; }
float outBack(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
}

float cubicBezier(float x1, float y1, float x2, float y2, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    auto bez = [](float a, float b, float s) {
        const float u = 1 - s;
        return 3 * u * u * s * a + 3 * u * s * s * b + s * s * s;
    };
    auto dbez = [](float a, float b, float s) {
        const float u = 1 - s;
        return 3 * u * u * a + 6 * u * s * (b - a) + 3 * s * s * (1 - b);
    };
    // Newton iterations on x(s) = t, with bisection fallback.
    float s = t;
    for (int i = 0; i < 6; ++i) {
        const float x = bez(x1, x2, s) - t;
        const float d = dbez(x1, x2, s);
        if (std::fabs(x) < 1e-5f) return bez(y1, y2, s);
        if (std::fabs(d) < 1e-6f) break;
        s -= x / d;
    }
    float lo = 0, hi = 1;
    s = t;
    for (int i = 0; i < 20; ++i) {
        const float x = bez(x1, x2, s);
        if (std::fabs(x - t) < 1e-5f) break;
        if (x < t) lo = s;
        else hi = s;
        s = (lo + hi) * 0.5f;
    }
    return bez(y1, y2, s);
}

float emphasized(float t) { return cubicBezier(0.05f, 0.7f, 0.1f, 1.0f, t); }
}  // namespace ease

void stepSpring(float& x, float& v, float dt, const SpringSpec& spec) {
    const float omega = 2.0f * gfx::kPi / std::max(0.01f, spec.response);
    const float zeta = std::max(0.0f, spec.damping);
    if (zeta < 0.999f) {
        const float wd = omega * std::sqrt(1 - zeta * zeta);
        const float a = x;
        const float b = (v + zeta * omega * x) / wd;
        const float e = std::exp(-zeta * omega * dt);
        const float c = std::cos(wd * dt), s = std::sin(wd * dt);
        const float nx = e * (a * c + b * s);
        const float nv = -zeta * omega * nx + e * (-a * wd * s + b * wd * c);
        x = nx;
        v = nv;
    } else if (zeta <= 1.001f) {
        const float e = std::exp(-omega * dt);
        const float k = v + omega * x;
        const float nx = (x + k * dt) * e;
        const float nv = (v - k * omega * dt) * e;
        x = nx;
        v = nv;
    } else {
        const float sq = std::sqrt(zeta * zeta - 1);
        const float r1 = -omega * (zeta - sq), r2 = -omega * (zeta + sq);
        const float c2 = (v - r1 * x) / (r2 - r1);
        const float c1 = x - c2;
        const float e1 = std::exp(r1 * dt), e2 = std::exp(r2 * dt);
        x = c1 * e1 + c2 * e2;
        v = c1 * r1 * e1 + c2 * r2 * e2;
    }
}

Timeline::Handle Timeline::animate(float duration, EasingFn easing, std::function<void(float)> onUpdate,
                                   std::function<void()> onDone, std::weak_ptr<void> owner) {
    const std::weak_ptr<void> empty;
    const bool hasOwner = owner.owner_before(empty) || empty.owner_before(owner);
    Item it{next_++, std::max(0.0f, duration), 0.0f, easing ? easing : ease::linear, std::move(onUpdate),
            std::move(onDone), std::move(owner), hasOwner, false};
    const Handle id = it.id;
    (updating_ ? adding_ : items_).push_back(std::move(it));
    return id;
}

Timeline::Handle Timeline::after(float delay, std::function<void()> fn, std::weak_ptr<void> owner) {
    return animate(delay, ease::linear, {}, std::move(fn), std::move(owner));
}

void Timeline::cancel(Handle h) {
    for (auto& it : items_)
        if (it.id == h) it.cancelled = true;
    for (auto& it : adding_)
        if (it.id == h) it.cancelled = true;
}

bool Timeline::active(Handle h) const {
    for (const auto& it : items_)
        if (it.id == h && !it.cancelled) return true;
    for (const auto& it : adding_)
        if (it.id == h && !it.cancelled) return true;
    return false;
}

void Timeline::update(float dt) {
    updating_ = true;
    for (size_t i = 0; i < items_.size(); ++i) {
        Item& it = items_[i];
        if (it.cancelled) continue;
        if (it.hasOwner && it.owner.expired()) {
            it.cancelled = true;
            continue;
        }
        it.elapsed += dt;
        const float t = it.duration <= 0 ? 1.0f : std::min(1.0f, it.elapsed / it.duration);
        // Called in place: while updating_, animate() appends to adding_ and clear()/cancel() only
        // flag items, so items_ is never reallocated or destroyed under the running callback.
        if (it.onUpdate) it.onUpdate(it.easing(t));
        if (t >= 1.0f && !it.cancelled) {
            it.cancelled = true;
            if (it.onDone) {
                auto done = std::move(it.onDone);
                done();
            }
        }
    }
    updating_ = false;
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& it) { return it.cancelled; }), items_.end());
    for (auto& it : adding_) items_.push_back(std::move(it));
    adding_.clear();
}

void Timeline::clear() {
    if (updating_) {
        for (auto& it : items_) it.cancelled = true;
        adding_.clear();
        return;
    }
    items_.clear();
    adding_.clear();
}

}  // namespace karafin::ui
