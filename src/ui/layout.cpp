#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>

namespace karafin::ui {

Box& Box::setAxis(Axis a) {
    axis_ = a;
    setNeedsLayout();
    return *this;
}
Box& Box::setPadding(const Insets& p) {
    padding_ = p;
    setNeedsLayout();
    return *this;
}
Box& Box::setSpacing(float s) {
    spacing_ = s;
    setNeedsLayout();
    return *this;
}
Box& Box::setAlign(Align a) {
    align_ = a;
    setNeedsLayout();
    return *this;
}
Box& Box::setJustify(Justify j) {
    justify_ = j;
    setNeedsLayout();
    return *this;
}

namespace {
float mainOf(Vec2 v, Axis a) { return a == Axis::Horizontal ? v.x : v.y; }
float crossOf(Vec2 v, Axis a) { return a == Axis::Horizontal ? v.y : v.x; }
float mainMargin(const Insets& m, Axis a) { return a == Axis::Horizontal ? m.horizontal() : m.vertical(); }
float crossMargin(const Insets& m, Axis a) { return a == Axis::Horizontal ? m.vertical() : m.horizontal(); }
}  // namespace

Vec2 Box::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    if (pref.x >= 0 && pref.y >= 0) return pref;
    const Vec2 inner{std::max(0.0f, (pref.x >= 0 ? pref.x : available.x) - padding_.horizontal()),
                     std::max(0.0f, (pref.y >= 0 ? pref.y : available.y) - padding_.vertical())};
    const float innerMain = mainOf(inner, axis_);
    const float innerCross = crossOf(inner, axis_);
    auto availFor = [&](const View* c, float mainAvail) {
        const float crossAvail = innerCross - crossMargin(c->margin(), axis_);
        return axis_ == Axis::Horizontal ? Vec2{mainAvail, crossAvail} : Vec2{crossAvail, mainAvail};
    };
    // Pass 1: fixed children. Pass 2: flex children measured with their share of the remaining
    // space (when bounded), so wrapped text inside them measures like it will lay out.
    float main = 0, cross = 0, flexTotal = 0;
    int count = 0;
    for (auto& cp : children()) {
        View* c = cp.get();
        if (!c->visible()) continue;
        ++count;
        if (c->flex() > 0) {
            flexTotal += c->flex();
            continue;
        }
        const Vec2 m = c->measure(availFor(c, innerMain));
        main += mainOf(m, axis_) + mainMargin(c->margin(), axis_);
        cross = std::max(cross, crossOf(m, axis_) + crossMargin(c->margin(), axis_));
    }
    if (count > 1) main += spacing_ * static_cast<float>(count - 1);
    if (flexTotal > 0) {
        const bool bounded = std::isfinite(innerMain);
        const float free = bounded ? std::max(0.0f, innerMain - main) : 0.0f;
        for (auto& cp : children()) {
            View* c = cp.get();
            if (!c->visible() || c->flex() <= 0) continue;
            const float share = bounded ? free * c->flex() / flexTotal - mainMargin(c->margin(), axis_) : innerMain;
            const Vec2 m = c->measure(availFor(c, share));
            main += (bounded ? share : mainOf(m, axis_)) + mainMargin(c->margin(), axis_);
            cross = std::max(cross, crossOf(m, axis_) + crossMargin(c->margin(), axis_));
        }
    }
    Vec2 out = axis_ == Axis::Horizontal ? Vec2{main, cross} : Vec2{cross, main};
    out.x += padding_.horizontal();
    out.y += padding_.vertical();
    if (pref.x >= 0) out.x = pref.x;
    if (pref.y >= 0) out.y = pref.y;
    return out;
}

void Box::layout() {
    const Rect content = bounds().inset(padding_);
    const float mainSize = axis_ == Axis::Horizontal ? content.w : content.h;
    const float crossSize = axis_ == Axis::Horizontal ? content.h : content.w;

    std::vector<View*> kids;
    std::vector<Vec2> sizes;
    float used = 0, flexTotal = 0;
    for (auto& cp : children()) {
        View* c = cp.get();
        if (!c->visible()) continue;
        const Vec2 avail = axis_ == Axis::Horizontal ? Vec2{mainSize, crossSize - crossMargin(c->margin(), axis_)}
                                                     : Vec2{crossSize - crossMargin(c->margin(), axis_), mainSize};
        Vec2 m = c->measure(avail);
        if (c->flex() > 0) {
            flexTotal += c->flex();
            if (axis_ == Axis::Horizontal) m.x = 0;
            else m.y = 0;
        }
        used += mainOf(m, axis_) + mainMargin(c->margin(), axis_);
        kids.push_back(c);
        sizes.push_back(m);
    }
    if (kids.empty()) return;
    used += spacing_ * static_cast<float>(kids.size() - 1);
    const float free = std::max(0.0f, mainSize - used);

    float pos = 0, gap = spacing_;
    if (flexTotal <= 0) {
        if (justify_ == Justify::Center) pos = free * 0.5f;
        else if (justify_ == Justify::End) pos = free;
        else if (justify_ == Justify::SpaceBetween && kids.size() > 1) gap += free / static_cast<float>(kids.size() - 1);
    }

    for (size_t i = 0; i < kids.size(); ++i) {
        View* c = kids[i];
        const Insets& mg = c->margin();
        float childMain = mainOf(sizes[i], axis_);
        if (c->flex() > 0 && flexTotal > 0) {
            childMain = free * c->flex() / flexTotal;
            if (align_ != Align::Stretch) {
                const float crossAvail = crossSize - crossMargin(mg, axis_);
                sizes[i] = c->measure(axis_ == Axis::Horizontal ? Vec2{childMain, crossAvail} : Vec2{crossAvail, childMain});
            }
        }
        const float availCross = crossSize - crossMargin(mg, axis_);
        float childCross = align_ == Align::Stretch ? availCross : std::min(crossOf(sizes[i], axis_), availCross);
        // Fixed preferred cross size wins over stretch.
        const float prefCross = axis_ == Axis::Horizontal ? c->preferredSize().y : c->preferredSize().x;
        if (align_ == Align::Stretch && prefCross >= 0) childCross = prefCross;
        float crossPos = 0;
        if (align_ == Align::Center) crossPos = (availCross - childCross) * 0.5f;
        else if (align_ == Align::End) crossPos = availCross - childCross;

        if (axis_ == Axis::Horizontal) {
            pos += mg.left;
            c->setFrame({content.x + pos, content.y + mg.top + crossPos, childMain, childCross});
            pos += childMain + mg.right + gap;
        } else {
            pos += mg.top;
            c->setFrame({content.x + mg.left + crossPos, content.y + pos, childCross, childMain});
            pos += childMain + mg.bottom + gap;
        }
    }
}

}  // namespace karafin::ui
