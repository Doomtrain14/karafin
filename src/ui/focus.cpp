#include "ui/focus.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace karafin::ui {
namespace {

void collectFocusable(View* v, std::vector<View*>& out) {
    if (!v->visible()) return;
    if (v->canFocus()) out.push_back(v);
    for (auto& c : v->children()) collectFocusable(c.get(), out);
}

}  // namespace

View* FocusManager::resolve(View* v) const {
    for (int depth = 0; v && depth < 8; ++depth) {
        if (v->canFocus()) return v;
        View* next = v->defaultFocus();
        if (next == v) return nullptr;
        v = next;
    }
    return v && v->canFocus() ? v : nullptr;
}

View* FocusManager::redirectToMemory(View* target, View* from) const {
    View* scope = scope_.get();
    std::vector<View*> chain;  // ancestors of target below the scope, outermost first
    for (View* p = target->parent(); p && p != scope; p = p->parent()) chain.insert(chain.begin(), p);
    for (View* a : chain) {
        if (!a->remembersFocus() || (from && from->isDescendantOf(a))) continue;
        View* m = a->rememberedFocus();
        if (m && m != target && m->isDescendantOf(a) && m->canFocus()) return m;
    }
    return target;
}

void FocusManager::apply(View* target) {
    View* old = focused_.get();
    if (target == old) return;
    // A destroyed focused view leaves a null Ref but its id is still in lastFocusId_.
    const std::string previousId = old ? old->focusId() : lastFocusId_;
    focused_ = target;
    if (old) {
        old->focused_ = false;
        old->onBlur();
        for (View* p = old->parent(); p; p = p->parent())
            if (!target || !target->isDescendantOf(p)) p->onFocusLeft();
    }
    if (target) {
        target->focused_ = true;
        target->focusGainSilent_ = !target->focusId().empty() && target->focusId() == previousId;
        target->onFocus();
        target->focusGainSilent_ = false;
        lastFocusId_ = target->focusId();
        for (View* p = target->parent(); p; p = p->parent()) {
            if (p->remembersFocus()) p->setRememberedFocus(target);
            p->onFocusWithin(target);
        }
    } else {
        lastFocusId_.clear();
    }
}

void FocusManager::setFocus(View* v) {
    if (!v) {
        apply(nullptr);
        return;
    }
    View* target = resolve(v);
    if (!target) return;
    if (View* scope = scope_.get(); scope && !target->isDescendantOf(scope)) return;
    apply(target);
}

bool FocusManager::navigate(NavDir dir) {
    View* scope = scope_.get();
    if (!scope) return false;
    View* from = focused_.get();
    if (!from) {
        setFocus(scope->defaultFocus());
        return focused_.get() != nullptr;
    }
    for (View* p = from->parent(); p; p = p->parent()) {
        View* t = p->navigateFocus(from, dir);
        if (t == View::focusBlocked()) return true;
        if (t) {
            View* resolved = resolve(t);
            if (resolved && resolved != from) {
                apply(resolved);
                return true;
            }
        }
        if (p == scope) break;
    }
    View* t = findInDirection(scope, from, dir);
    if (!t) return false;
    t = redirectToMemory(t, from);
    apply(t);
    return true;
}

void FocusManager::setScope(View* scope) {
    if (scope_.get() == scope) return;
    scope_ = scope;
    if (!scope) return;
    View* f = focused_.get();
    if (f && f->isDescendantOf(scope) && f->canFocus()) return;
    apply(resolve(scope->defaultFocus()));
}

void FocusManager::validate() {
    View* scope = scope_.get();
    if (!scope) return;
    View* f = focused_.get();
    if (f && f->isDescendantOf(scope) && f->canFocus()) return;
    if (!f) focused_.reset();
    View* next = resolve(scope->defaultFocus());
    if (next != f) apply(next);
}

void FocusManager::clear() {
    apply(nullptr);
    scope_.reset();
}

View* FocusManager::findInDirection(View* scope, View* from, NavDir dir) {
    if (!scope || !from) return nullptr;
    std::vector<View*> candidates;
    collectFocusable(scope, candidates);
    const Rect cur = from->rootRect();
    const Vec2 cc = cur.center();
    View* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    for (View* v : candidates) {
        if (v == from || v->isDescendantOf(from) || from->isDescendantOf(v)) continue;
        const Rect r = v->rootRect();
        const Vec2 rc = r.center();
        float major = 0, minor = 0, overlap = 0;
        bool ok = false;
        switch (dir) {
            case NavDir::Right:
                ok = rc.x > cc.x + 1 && r.right() > cur.right() - 1;
                major = std::max(0.0f, r.x - cur.right());
                minor = std::fabs(rc.y - cc.y);
                overlap = std::min(cur.bottom(), r.bottom()) - std::max(cur.y, r.y);
                break;
            case NavDir::Left:
                ok = rc.x < cc.x - 1 && r.x < cur.x + 1;
                major = std::max(0.0f, cur.x - r.right());
                minor = std::fabs(rc.y - cc.y);
                overlap = std::min(cur.bottom(), r.bottom()) - std::max(cur.y, r.y);
                break;
            case NavDir::Down:
                ok = rc.y > cc.y + 1 && r.bottom() > cur.bottom() - 1;
                major = std::max(0.0f, r.y - cur.bottom());
                minor = std::fabs(rc.x - cc.x);
                overlap = std::min(cur.right(), r.right()) - std::max(cur.x, r.x);
                break;
            case NavDir::Up:
                ok = rc.y < cc.y - 1 && r.y < cur.y + 1;
                major = std::max(0.0f, cur.y - r.bottom());
                minor = std::fabs(rc.x - cc.x);
                overlap = std::min(cur.right(), r.right()) - std::max(cur.x, r.x);
                break;
        }
        if (!ok) continue;
        const bool inBeam = overlap > 0;
        // Items sharing the beam win unless much farther; off-beam items pay for misalignment.
        const float score = major + minor * (inBeam ? 0.6f : 2.5f) + (inBeam ? 0.0f : 40.0f);
        if (score < bestScore) {
            bestScore = score;
            best = v;
        }
    }
    return best;
}

}  // namespace karafin::ui
