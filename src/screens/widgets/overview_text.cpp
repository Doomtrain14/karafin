#include "screens/widgets/overview_text.hpp"

#include <algorithm>
#include <limits>
#include <memory>

#include "gfx/text.hpp"
#include "ui/modal.hpp"
#include "ui/root.hpp"
#include "ui/scroll.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
constexpr float kSheetW = 760;
constexpr float kSheetMaxH = 600;
constexpr float kSheetPad = 32;
constexpr float kSheetGap = 16;

class TextSheet : public Modal {
public:
    TextSheet(std::string title, std::string text) {
        setDim(0.62f);
        title_ = add<Label>(std::move(title), theme::type::title2);
        title_->setMaxLines(2);
        scroll_ = add<ScrollView>(Axis::Vertical);
        scroll_->setClipsChildren(true);
        scroll_->setShowsIndicator(true);
        body_ = scroll_->content()->add<Label>(std::move(text), theme::type::body, theme::color::textSecondary);
        body_->setMaxLines(0);
        body_->setPreferredSize(kSheetW - 2 * kSheetPad, -1);
    }

    bool onInput(const InputEvent& e) override {
        if (e.pressedOrRepeat()) {
            const float line = gfx::text::lineHeight(theme::type::body);
            const float page = std::max(line, scroll_->viewportSize() - line);
            float delta = 0;
            switch (e.button) {
                case Key::Up: delta = -3 * line; break;
                case Key::Down: delta = 3 * line; break;
                case Key::ZL: delta = -page; break;
                case Key::ZR: delta = page; break;
                default: break;
            }
            if (delta != 0) {
                scroll_->scrollBy(delta);
                return true;
            }
        }
        return Modal::onInput(e);
    }

    bool onTap(Vec2 local) override {
        if (card_.contains(local)) return true;
        return Modal::onTap(local);
    }

    void layout() override {
        const float innerW = kSheetW - 2 * kSheetPad;
        const float inf = std::numeric_limits<float>::infinity();
        const float titleH = title_->measure({innerW, inf}).y;
        const float bodyH = body_->measure({innerW, inf}).y;
        const float maxBodyH = std::min(kSheetMaxH, height() - 80) - 2 * kSheetPad - titleH - kSheetGap;
        const float viewH = std::min(bodyH, maxBodyH);
        const float h = 2 * kSheetPad + titleH + kSheetGap + viewH;
        card_ = {(width() - kSheetW) * 0.5f, (height() - h) * 0.5f, kSheetW, h};
        title_->setFrame({card_.x + kSheetPad, card_.y + kSheetPad, innerW, titleH});
        scroll_->setFrame({card_.x + kSheetPad, card_.y + kSheetPad + titleH + kSheetGap, innerW, viewH});
    }

    void update(float dt) override {
        Modal::update(dt);
        const float a = std::clamp(appear(), 0.0f, 1.0f);
        title_->alpha.set(a);
        scroll_->alpha.set(a);
    }

    void draw(Renderer& r) override {
        const float a = std::clamp(appear(), 0.0f, 1.0f);
        const float s = 0.94f + 0.06f * a;
        r.pushTransform(gfx::Affine::scaleAround(s, card_.center()));
        r.pushAlpha(a);
        r.drawShadow(card_, theme::radius::xl, 60, Color::black(0.65f), {0, 24});
        r.fillRoundRect(card_, theme::radius::xl, gfx::Gradient::vertical(Color::hex(0x23232F), Color::hex(0x1A1A24)));
        r.strokeRoundRect(card_, theme::radius::xl, 1, theme::color::stroke);
        r.popAlpha();
        r.popTransform();
    }

private:
    Label* title_ = nullptr;
    ScrollView* scroll_ = nullptr;
    Label* body_ = nullptr;
    Rect card_{};
};
}  // namespace

void presentTextSheet(std::string title, std::string text) {
    present(std::make_unique<TextSheet>(std::move(title), std::move(text)));
}

OverviewText::OverviewText(int maxLines)
    : Label({}, theme::type::body, theme::color::textSecondary), maxLines_(maxLines) {
    setMaxLines(maxLines);
    setHints({{Key::A, "More"}});
    onActivate = [this] { presentTextSheet(sheetTitle_, text()); };
}

void OverviewText::layout() {
    Label::layout();
    gfx::TextOptions o;
    o.maxWidth = width();
    o.maxLines = maxLines_;
    truncated_ = width() > 0 && !text().empty() && gfx::text::layout(text(), style(), o)->truncated;
    setFocusable(truncated_);
}

void OverviewText::update(float dt) {
    focus_.update(dt);
    Label::update(dt);
}

void OverviewText::draw(Renderer& r) {
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    if (f > 0.01f) {
        const Rect plate = bounds().expanded(10);
        r.fillRoundRect(plate, theme::radius::md, Color::white(0.08f * f));
        r.strokeRoundRect(plate, theme::radius::md, 2, theme::color::focusRing.withAlpha(theme::color::focusRing.a * f));
    }
    Label::draw(r);
}

void OverviewText::onFocus() { focus_.springTo(1.0f, theme::motion::snappy); }
void OverviewText::onBlur() { focus_.springTo(0.0f, theme::motion::snappy); }

}  // namespace karafin::screens
