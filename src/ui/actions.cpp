#include "ui/actions.hpp"

#include <algorithm>

#include "ui/root.hpp"
#include "ui/widgets/list_menu.hpp"

namespace karafin::ui {

void ActionMenu::setTitle(std::string title, std::string subtitle) {
    if (!title_.empty()) return;
    title_ = std::move(title);
    subtitle_ = std::move(subtitle);
}

ActionMenu& ActionMenu::section(const std::string& heading) {
    for (size_t i = 0; i < groups_.size(); ++i)
        if (groups_[i].heading == heading) {
            current_ = i;
            return *this;
        }
    groups_.push_back({heading, {}});
    current_ = groups_.size() - 1;
    return *this;
}

ActionMenu& ActionMenu::add(std::string title, gfx::Icon icon, std::function<void()> run, std::string detail) {
    Action a;
    a.title = std::move(title);
    a.icon = icon;
    a.run = std::move(run);
    a.detail = std::move(detail);
    return add(std::move(a));
}

ActionMenu& ActionMenu::add(Action action) {
    if (groups_.empty()) section({});
    groups_[current_].actions.push_back(std::move(action));
    return *this;
}

bool ActionMenu::empty() const {
    return std::all_of(groups_.begin(), groups_.end(), [](const Group& g) { return g.actions.empty(); });
}

namespace {

class ActionSheet final : public SideSheet {
public:
    ActionSheet(std::string title, std::string subtitle, Key toggleKey)
        : SideSheet(std::move(title), std::move(subtitle)), toggleKey_(toggleKey) {}

    bool onInput(const InputEvent& e) override {
        if (e.button == toggleKey_ && e.pressed()) {
            dismiss();
            return true;
        }
        return SideSheet::onInput(e);
    }

private:
    Key toggleKey_;
};

}  // namespace

bool presentActions(ActionMenu menu, Key toggleKey) {
    if (menu.empty()) return false;
    std::vector<ListItem> items;
    int focus = -1;
    for (const auto& g : menu.groups()) {
        if (g.actions.empty()) continue;
        if (!g.heading.empty()) {
            ListItem h;
            h.title = g.heading;
            h.heading = true;
            h.enabled = false;
            items.push_back(std::move(h));
        }
        for (const auto& a : g.actions) {
            ListItem it;
            it.title = a.title;
            it.subtitle = a.subtitle;
            it.detail = a.detail;
            it.icon = a.icon;
            it.enabled = a.enabled && static_cast<bool>(a.run);
            it.destructive = a.destructive;
            it.checked = a.checked;
            // The sheet dismisses right after this (SideSheet::onSelect); run on the next frame with
            // the screen restored as the focus scope (focus back on the item the menu was opened
            // from), so actions can move focus and see hasFocusWithin() as before.
            if (it.enabled)
                it.onSelect = [run = a.run] {
                    timeline().after(0.0f, [run] {
                        root().updateScope();
                        run();
                    });
                };
            if (it.enabled && ((focus < 0 && !it.destructive) || a.checked)) focus = static_cast<int>(items.size());
            items.push_back(std::move(it));
        }
    }
    std::string title = menu.title().empty() ? std::string("Actions") : menu.title();
    auto sheet = std::make_unique<ActionSheet>(std::move(title), menu.subtitle(), toggleKey);
    sheet->menu()->setItems(std::move(items), focus);
    if (menu.titleArt()) sheet->setTitleArt(menu.titleArtHeight(), menu.titleArt());
    present(std::move(sheet));
    return true;
}

}  // namespace karafin::ui
