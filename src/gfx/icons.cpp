#include "gfx/icons.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "gfx/renderer.hpp"

namespace karafin::gfx {
namespace {

// Maps the 24-unit design grid into the target box.
struct Pen {
    Renderer& r;
    Vec2 origin;
    float s;
    Color c;
    float w;  // stroke width in grid units

    Vec2 p(float x, float y) const { return {origin.x + x * s, origin.y + y * s}; }
    void line(float x1, float y1, float x2, float y2) const { r.drawLine(p(x1, y1), p(x2, y2), w * s, c); }
    void poly(std::initializer_list<Vec2> pts) const {
        const Vec2* prev = nullptr;
        for (const Vec2& q : pts) {
            if (prev) line(prev->x, prev->y, q.x, q.y);
            prev = &q;
        }
    }
    void dot(float x, float y, float rad) const { r.fillCircle(p(x, y), rad * s, c); }
    void ring(float x, float y, float rad) const { r.strokeCircle(p(x, y), rad * s, w * s, c); }
    void rect(float x, float y, float ww, float hh, float rad) const {
        r.fillRoundRect({origin.x + x * s, origin.y + y * s, ww * s, hh * s}, rad * s, c);
    }
    void rectStroke(float x, float y, float ww, float hh, float rad) const {
        r.strokeRoundRect({origin.x + x * s, origin.y + y * s, ww * s, hh * s}, rad * s, w * s, c);
    }
    // Triangle with base `bw`, height `h`, apex pointing at `angle` (0 = up, clockwise radians).
    void tri(float cx, float cy, float bw, float h, float angle, float rad = 1.2f) const {
        r.fillTriangle(p(cx, cy), {bw * s, h * s}, angle, c, rad * s);
    }
    // Centerline radius `rad`; angles in degrees, 0 = 3 o'clock, clockwise.
    void arc(float x, float y, float rad, float startDeg, float sweepDeg) const {
        r.drawArc(p(x, y), rad * s, w * s, startDeg * kPi / 180.0f, sweepDeg * kPi / 180.0f, c);
    }
};

constexpr float kRight = kPi * 0.5f;
constexpr float kDown = kPi;
constexpr float kLeft = -kPi * 0.5f;

void seekIcon(const Pen& pen, bool forward) {
    // Open circle with an arrowhead at the top, and a "10" in the middle.
    if (forward) {
        pen.arc(12, 13, 8, -60, 300);
        pen.tri(13.6f, 5.0f, 6.0f, 5.0f, kRight, 0.8f);
    } else {
        pen.arc(12, 13, 8, -60 - 180, 300);
        pen.tri(10.4f, 5.0f, 6.0f, 5.0f, kLeft, 0.8f);
    }
    TextStyle st;
    st.size = 7.5f * pen.s;
    st.weight = FontWeight::Bold;
    st.lineHeight = 1.0f;
    auto layout = text::layout("10", st);
    const Vec2 c = pen.p(12, 13.2f);
    pen.r.drawText(*layout, {c.x - layout->width * 0.5f, c.y - layout->height * 0.5f}, pen.c);
}

}  // namespace

void drawIcon(Renderer& r, Icon icon, const Rect& box, Color color, float weight) {
    const float size = std::min(box.w, box.h);
    const Vec2 origin{box.x + (box.w - size) * 0.5f, box.y + (box.h - size) * 0.5f};
    const Pen pen{r, origin, size / 24.0f, color, 2.0f * weight};

    switch (icon) {
        case Icon::None:
        case Icon::Count:
            break;
        case Icon::Play:
            pen.tri(13.2f, 12, 15, 13.5f, kRight, 1.6f);
            break;
        case Icon::Pause:
            pen.rect(6, 5, 4.2f, 14, 1.3f);
            pen.rect(13.8f, 5, 4.2f, 14, 1.3f);
            break;
        case Icon::Stop:
            pen.rect(6, 6, 12, 12, 2.2f);
            break;
        case Icon::SkipNext:
            pen.tri(10.5f, 12, 13, 11, kRight, 1.2f);
            pen.rect(16, 5.5f, 2.6f, 13, 1.0f);
            break;
        case Icon::SkipPrevious:
            pen.tri(13.5f, 12, 13, 11, kLeft, 1.2f);
            pen.rect(5.4f, 5.5f, 2.6f, 13, 1.0f);
            break;
        case Icon::FastForward:
            pen.tri(8.0f, 12, 12, 9, kRight, 1.0f);
            pen.tri(16.0f, 12, 12, 9, kRight, 1.0f);
            break;
        case Icon::Rewind:
            pen.tri(8.0f, 12, 12, 9, kLeft, 1.0f);
            pen.tri(16.0f, 12, 12, 9, kLeft, 1.0f);
            break;
        case Icon::SeekForward:
            seekIcon(pen, true);
            break;
        case Icon::SeekBack:
            seekIcon(pen, false);
            break;
        case Icon::Download:
            pen.line(12, 4, 12, 15);
            pen.poly({{7, 10.5f}, {12, 15.5f}, {17, 10.5f}});
            pen.line(5, 20, 19, 20);
            break;
        case Icon::Check:
            pen.poly({{5, 12.5f}, {9.8f, 17.3f}, {19, 7.2f}});
            break;
        case Icon::Close:
            pen.line(6.5f, 6.5f, 17.5f, 17.5f);
            pen.line(17.5f, 6.5f, 6.5f, 17.5f);
            break;
        case Icon::Plus:
            pen.line(12, 5, 12, 19);
            pen.line(5, 12, 19, 12);
            break;
        case Icon::Minus:
            pen.line(5, 12, 19, 12);
            break;
        case Icon::ChevronLeft:
            pen.poly({{14.5f, 5.5f}, {8, 12}, {14.5f, 18.5f}});
            break;
        case Icon::ChevronRight:
            pen.poly({{9.5f, 5.5f}, {16, 12}, {9.5f, 18.5f}});
            break;
        case Icon::ChevronUp:
            pen.poly({{5.5f, 14.5f}, {12, 8}, {18.5f, 14.5f}});
            break;
        case Icon::ChevronDown:
            pen.poly({{5.5f, 9.5f}, {12, 16}, {18.5f, 9.5f}});
            break;
        case Icon::Back:
            pen.line(19, 12, 5.5f, 12);
            pen.poly({{11, 6}, {5, 12}, {11, 18}});
            break;
        case Icon::Subtitles:
            pen.rectStroke(2.5f, 4.5f, 19, 15, 3.2f);
            pen.line(6.5f, 12, 10, 12);
            pen.line(13, 12, 17.5f, 12);
            pen.line(6.5f, 15.5f, 14, 15.5f);
            pen.line(17, 15.5f, 17.5f, 15.5f);
            break;
        case Icon::Audio:
            pen.rect(3.5f, 9, 4, 6, 1.0f);
            pen.tri(9.0f, 12, 14, 8, kLeft, 1.0f);
            pen.arc(12, 12, 4.5f, -45, 90);
            pen.arc(12, 12, 8.5f, -50, 100);
            break;
        case Icon::Settings: {
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * kPi / 4.0f;
                const float cs = std::cos(a), sn = std::sin(a);
                Pen tooth = pen;
                tooth.w = 3.4f * weight;
                tooth.line(12 + cs * 7.6f, 12 + sn * 7.6f, 12 + cs * 9.4f, 12 + sn * 9.4f);
            }
            Pen body = pen;
            body.w = 3.2f * weight;
            body.ring(12, 12, 6.6f);
            pen.ring(12, 12, 2.6f);
            break;
        }
        case Icon::Search:
            pen.ring(10.5f, 10.5f, 6.8f);
            pen.line(15.6f, 15.6f, 20, 20);
            break;
        case Icon::Home:
            pen.poly({{3.5f, 11.5f}, {12, 4}, {20.5f, 11.5f}});
            pen.poly({{6, 10}, {6, 20}, {18, 20}, {18, 10}});
            pen.rect(10, 14, 4, 6, 1.0f);
            break;
        case Icon::Library:
            pen.rect(4, 4, 7, 7, 2.0f);
            pen.rect(13, 4, 7, 7, 2.0f);
            pen.rect(4, 13, 7, 7, 2.0f);
            pen.rect(13, 13, 7, 7, 2.0f);
            break;
        case Icon::Movie:
            pen.rectStroke(3, 5, 18, 14, 2.6f);
            pen.tri(12.6f, 12, 7, 6.5f, kRight, 0.8f);
            break;
        case Icon::Tv:
            pen.rectStroke(2.5f, 4.5f, 19, 13, 2.4f);
            pen.line(8, 21, 16, 21);
            break;
        case Icon::User:
            pen.ring(12, 8, 4.2f);
            pen.arc(12, 21.5f, 7.5f, 180, 180);
            break;
        case Icon::Server:
            pen.rectStroke(3, 3.5f, 18, 7.5f, 2.0f);
            pen.rectStroke(3, 13, 18, 7.5f, 2.0f);
            pen.dot(7, 7.25f, 1.3f);
            pen.dot(7, 16.75f, 1.3f);
            break;
        case Icon::Refresh:
            pen.arc(12, 12, 7.5f, -60, 290);
            pen.tri(17.0f, 4.8f, 6.0f, 5.0f, kRight + 0.5f, 0.8f);
            break;
        case Icon::Trash:
            pen.line(4, 6.5f, 20, 6.5f);
            pen.poly({{9.5f, 6.5f}, {9.5f, 3.8f}, {14.5f, 3.8f}, {14.5f, 6.5f}});
            pen.poly({{6.5f, 9}, {7.5f, 20}, {16.5f, 20}, {17.5f, 9}});
            break;
        case Icon::Info:
            pen.ring(12, 12, 9.5f);
            pen.line(12, 11, 12, 16.8f);
            pen.dot(12, 7.6f, 1.4f);
            break;
        case Icon::Warning:
            pen.poly({{12, 3.5f}, {21, 19.5f}, {3, 19.5f}, {12, 3.5f}});
            pen.line(12, 9.5f, 12, 14);
            pen.dot(12, 16.8f, 1.3f);
            break;
        case Icon::Star:
            for (int i = 0; i < 5; ++i) {
                const float a = static_cast<float>(i) * 2.0f * kPi / 5.0f;
                pen.tri(12 + std::sin(a) * 5.2f, 12.8f - std::cos(a) * 5.2f, 6.6f, 8.0f, a, 0.6f);
            }
            pen.dot(12, 12.8f, 4.4f);
            break;
        case Icon::Heart:
            pen.dot(8.4f, 9.6f, 4.6f);
            pen.dot(15.6f, 9.6f, 4.6f);
            pen.tri(12, 14.6f, 17.2f, 9.6f, kDown, 1.2f);
            break;
        case Icon::More:
            pen.dot(5.5f, 12, 2.0f);
            pen.dot(12, 12, 2.0f);
            pen.dot(18.5f, 12, 2.0f);
            break;
        case Icon::Menu:
            pen.line(4, 6.5f, 20, 6.5f);
            pen.line(4, 12, 20, 12);
            pen.line(4, 17.5f, 20, 17.5f);
            break;
        case Icon::List:
            pen.dot(5, 6.5f, 1.4f);
            pen.dot(5, 12, 1.4f);
            pen.dot(5, 17.5f, 1.4f);
            pen.line(9, 6.5f, 20, 6.5f);
            pen.line(9, 12, 20, 12);
            pen.line(9, 17.5f, 20, 17.5f);
            break;
        case Icon::Lock:
            pen.rect(5, 10.5f, 14, 10.5f, 2.2f);
            pen.arc(12, 10.5f, 4.4f, 180, 180);
            pen.line(7.6f, 10.5f, 7.6f, 11);
            pen.line(16.4f, 10.5f, 16.4f, 11);
            break;
        case Icon::Wifi:
            pen.arc(12, 19, 14, -135, 90);
            pen.arc(12, 19, 9, -135, 90);
            pen.arc(12, 19, 4.2f, -135, 90);
            pen.dot(12, 19, 1.5f);
            break;
        case Icon::Filter:
            pen.line(4, 6.5f, 20, 6.5f);
            pen.line(7, 12, 17, 12);
            pen.line(10, 17.5f, 14, 17.5f);
            break;
        case Icon::Clock:
            pen.ring(12, 12, 9.2f);
            pen.poly({{12, 7}, {12, 12}, {15.5f, 14}});
            break;
        case Icon::Exit:
            pen.poly({{13, 4.5f}, {5, 4.5f}, {5, 19.5f}, {13, 19.5f}});
            pen.line(10, 12, 20, 12);
            pen.poly({{16.5f, 8.5f}, {20, 12}, {16.5f, 15.5f}});
            break;
        case Icon::Speed:
            pen.arc(12, 14, 8.5f, 160, 220);
            pen.line(12, 14, 16.2f, 9.2f);
            pen.dot(12, 14, 1.8f);
            break;
    }
}

const char* iconName(Icon icon) {
    static const char* const kNames[] = {
        "none",     "play",    "pause",  "stop",        "skip-next",    "skip-previous", "fast-forward", "rewind",
        "seek-fwd", "seek-back", "download", "check",   "close",        "plus",          "minus",        "chevron-left",
        "chevron-right", "chevron-up", "chevron-down", "back", "subtitles", "audio", "settings", "search",
        "home",     "library", "movie",  "tv",          "user",         "server",        "refresh",      "trash",
        "info",     "warning", "star",   "heart",       "more",         "menu",          "list",         "lock",
        "wifi",     "filter",  "clock",  "exit",        "speed",
    };
    const auto i = static_cast<size_t>(icon);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

}  // namespace karafin::gfx
