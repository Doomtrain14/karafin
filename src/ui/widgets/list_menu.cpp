#include "ui/widgets/list_menu.hpp"

#include <algorithm>

#include "gfx/text.hpp"
#include "ui/root.hpp"

namespace karafin::ui {
namespace {
constexpr float kRowHeight = 54;
constexpr float kRowHeightTall = 66;
constexpr float kRowPadX = 18;
}  // namespace

class ListMenu::Row : public View {
public:
    Row(ListMenu* menu, size_t index) : menu_(menu), index_(index) {
        setFocusable(true);
        setCullMargin(4);
    }

    Vec2 measure(Vec2 available) override {
        const ListItem& it = menu_->items_[index_];
        return {available.x, it.subtitle.empty() ? kRowHeight : kRowHeightTall};
    }

    void onFocus() override { focus_.springTo(1.0f, theme::motion::snappy); }
    void onBlur() override { focus_.springTo(0.0f, theme::motion::snappy); }
    bool activate() override {
        if (!menu_->items_[index_].enabled) return false;
        menu_->select(index_);
        return true;
    }
    void update(float dt) override {
        focus_.update(dt);
        View::update(dt);
    }

    void draw(Renderer& r) override {
        using namespace theme;
        const ListItem& it = menu_->items_[index_];
        const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
        const float alphaMul = it.enabled ? 1.0f : 0.4f;
        const Color ink = Color::lerp(color::textPrimary, color::textOnLight, f).mulAlpha(alphaMul);
        const Color sub = Color::lerp(color::textSecondary, Color(0.04f, 0.04f, 0.07f, 0.62f), f).mulAlpha(alphaMul);
        float x = kRowPadX;
        const float h = height();
        if (it.icon != gfx::Icon::None) {
            gfx::drawIcon(r, it.icon, {x, (h - 22) * 0.5f, 22, 22}, ink);
            x += 22 + 14;
        }
        float right = width() - kRowPadX;
        if (it.checked) {
            const Color check = Color::lerp(color::accentBright, color::accentDeep, f).mulAlpha(alphaMul);
            gfx::drawIcon(r, gfx::Icon::Check, {right - 22, (h - 22) * 0.5f, 22, 22}, check, 1.2f);
            right -= 22 + 12;
        }
        if (!it.detail.empty()) {
            // The detail keeps its natural width up to half the row; the title ellipsizes first.
            const float maxD = std::max(40.0f, (right - x - 12) * 0.5f);
            auto d = gfx::text::layout(it.detail, type::subhead, {.maxWidth = maxD});
            right -= d->width;
            r.drawText(*d, {right, (h - d->height) * 0.5f}, sub);
            right -= 12;
        }
        const float maxW = std::max(10.0f, right - x);
        auto title = gfx::text::layout(it.title, type::headline, {.maxWidth = maxW});
        if (it.subtitle.empty()) {
            r.drawText(*title, {x, (h - title->height) * 0.5f}, ink);
        } else {
            auto s = gfx::text::layout(it.subtitle, type::caption, {.maxWidth = maxW});
            const float total = title->height + s->height;
            const float y = (h - total) * 0.5f;
            r.drawText(*title, {x, y}, ink);
            r.drawText(*s, {x, y + title->height}, sub);
        }
    }

private:
    ListMenu* menu_;
    size_t index_;
    Animated<float> focus_{0.0f};
};

ListMenu::ListMenu() : ScrollView(Axis::Vertical) {
    setRememberFocus(true);
    setRevealInsets(12, 12);
    setShowsIndicator(true);
    content()->setSpacing(2);
    content()->setPadding({0, 6, 0, 6});
    setClipsChildren(true);
}

void ListMenu::setItems(std::vector<ListItem> items, int focusIndex) {
    content()->clearChildren();
    rows_.clear();
    items_ = std::move(items);
    for (size_t i = 0; i < items_.size(); ++i) rows_.push_back(content()->add<Row>(this, i));
    initialFocus_ = focusIndex;
    if (initialFocus_ < 0) {
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].checked) {
                initialFocus_ = static_cast<int>(i);
                break;
            }
    }
    hlAlpha_.set(0.0f);
    setNeedsLayout();
}

void ListMenu::setChecked(size_t index) {
    for (size_t i = 0; i < items_.size(); ++i) items_[i].checked = i == index;
}

void ListMenu::focusItem(size_t index) {
    if (index < rows_.size() && Root::exists()) Root::instance().focus().setFocus(rows_[index]);
}

View* ListMenu::defaultFocus() {
    if (View* m = rememberedFocus(); m && m->isDescendantOf(this) && m->canFocus()) return m;
    if (initialFocus_ >= 0 && static_cast<size_t>(initialFocus_) < rows_.size() &&
        rows_[static_cast<size_t>(initialFocus_)]->canFocus())
        return rows_[static_cast<size_t>(initialFocus_)];
    return ScrollView::defaultFocus();
}

void ListMenu::select(size_t index) {
    if (index >= items_.size()) return;
    auto fn = items_[index].onSelect;
    if (fn) fn();
    if (onSelect) onSelect(index);
}

void ListMenu::update(float dt) {
    ScrollView::update(dt);
    // Highlight follows the focused row (content coordinates).
    Row* focusedRow = nullptr;
    for (Row* row : rows_)
        if (row->focused()) focusedRow = row;
    if (focusedRow) {
        const Vec2 pos{focusedRow->frame().x, focusedRow->frame().y};
        const Vec2 size{focusedRow->frame().w, focusedRow->frame().h};
        if (hlAlpha_.target() < 0.5f && hlAlpha_.value() < 0.05f) {
            hlPos_.set(pos);
            hlSize_.set(size);
        } else {
            hlPos_.springTo(pos, theme::motion::snappy);
            hlSize_.springTo(size, theme::motion::snappy);
        }
        hlAlpha_.springTo(1.0f, theme::motion::snappy);
    } else {
        hlAlpha_.springTo(0.0f, theme::motion::snappy);
    }
    hlPos_.update(dt);
    hlSize_.update(dt);
    hlAlpha_.update(dt);
}

void ListMenu::draw(Renderer& r) {
    const float a = std::clamp(hlAlpha_.value(), 0.0f, 1.0f);
    if (a <= 0.01f) return;
    const Vec2 off = contentOffset();
    const Rect hl{hlPos_.value().x - off.x, hlPos_.value().y - off.y + content()->frame().y, hlSize_.value().x,
                  hlSize_.value().y};
    r.pushClip(bounds());
    r.drawShadow(hl, theme::radius::md, 18, Color::black(0.35f * a), {0, 6});
    r.fillRoundRect(hl, theme::radius::md, Color::white(0.95f * a));
    r.popClip();
}

// ---------------------------------------------------------------------------------------------

SideSheet::SideSheet(std::string title, std::string subtitle) {
    setDim(0.5f);
    panel_ = add<View>();
    title_ = panel_->add<Label>(std::move(title), theme::type::title2);
    subtitle_ = panel_->add<Label>(std::move(subtitle), theme::type::subhead, theme::color::textSecondary);
    menu_ = panel_->add<ListMenu>();
    menu_->onSelect = [this](size_t) {
        if (dismissOnSelect_) dismiss();
    };
}

std::unique_ptr<SideSheet> SideSheet::make(std::string title, std::vector<ListItem> items, int focusIndex,
                                           std::string subtitle) {
    auto sheet = std::make_unique<SideSheet>(std::move(title), std::move(subtitle));
    sheet->menu()->setItems(std::move(items), focusIndex);
    return sheet;
}

void SideSheet::layout() {
    const float w = panelWidth_;
    panel_->setFrame({width() - w, 0, w, height()});
    const float pad = 28;
    title_->setFrame({pad, 44, w - pad * 2, 30});
    float y = 44 + 32;
    if (!subtitle_->text().empty()) {
        subtitle_->setFrame({pad, y, w - pad * 2, 20});
        y += 26;
    }
    y += 14;
    menu_->setFrame({pad - 8, y, w - (pad - 8) * 2, height() - y - 32});
}

void SideSheet::update(float dt) {
    Modal::update(dt);
    const float a = appear();
    panel_->offset.set({(1.0f - a) * (panelWidth_ + 40), 0});
}

void SideSheet::draw(Renderer& r) {
    // Glass panel with a soft edge shadow (drawn here so it moves with the panel offset).
    const Rect p = panel_->frame().translated(panel_->offset.value());
    r.drawShadow(p, 0, 50, Color::black(0.6f * std::clamp(appear(), 0.0f, 1.0f)), {-10, 0});
    r.fillRect(p, gfx::Gradient::horizontal(Color::hex(0x16161F, 0.97f), Color::hex(0x101017, 0.98f)));
    r.fillRect({p.x, p.y, 1, p.h}, theme::color::stroke);
}

}  // namespace karafin::ui
