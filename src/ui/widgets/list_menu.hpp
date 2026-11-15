#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/icons.hpp"
#include "ui/modal.hpp"
#include "ui/scroll.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::ui {

struct ListItem {
    std::string title;
    std::string subtitle;  // second line (e.g. "AAC 5.1 - English")
    std::string detail;    // right-aligned value (e.g. "1.25x")
    gfx::Icon icon = gfx::Icon::None;
    bool checked = false;  // shows a check mark (radio-style menus)
    bool enabled = true;
    std::function<void()> onSelect;
};

// Vertical menu with a white highlight that glides between rows (text inverts underneath).
class ListMenu : public ScrollView {
public:
    ListMenu();

    void setItems(std::vector<ListItem> items, int focusIndex = -1);
    size_t itemCount() const { return items_.size(); }
    const ListItem& item(size_t i) const { return items_[i]; }
    // Radio behaviour: marks `index` checked and unchecks the others.
    void setChecked(size_t index);
    void focusItem(size_t index);

    // After the item's own onSelect.
    std::function<void(size_t index)> onSelect;

    void update(float dt) override;
    void draw(Renderer& r) override;
    View* defaultFocus() override;

private:
    class Row;
    friend class Row;
    void select(size_t index);

    std::vector<ListItem> items_;
    std::vector<Row*> rows_;
    Animated<Vec2> hlPos_{Vec2{0, 0}};
    Animated<Vec2> hlSize_{Vec2{0, 0}};
    Animated<float> hlAlpha_{0.0f};
    int initialFocus_ = -1;
};

// Panel sliding in from the right with a title and a ListMenu (track selection, options).
//
//   std::vector<ListItem> items = ...;
//   ui::present(SideSheet::make("Subtitles", std::move(items), currentIndex));
class SideSheet : public Modal {
public:
    SideSheet(std::string title, std::string subtitle = {});
    static std::unique_ptr<SideSheet> make(std::string title, std::vector<ListItem> items, int focusIndex = -1,
                                           std::string subtitle = {});

    ListMenu* menu() const { return menu_; }
    void setDismissOnSelect(bool v) { dismissOnSelect_ = v; }
    void setPanelWidth(float w) { panelWidth_ = w; }

    void layout() override;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    View* panel_ = nullptr;
    Label* title_ = nullptr;
    Label* subtitle_ = nullptr;
    ListMenu* menu_ = nullptr;
    float panelWidth_ = 440;
    bool dismissOnSelect_ = true;
};

}  // namespace karafin::ui
