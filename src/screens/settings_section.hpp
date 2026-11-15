#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "screens/main_screen.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/list_menu.hpp"

namespace karafin::screens {

// Settings: categories on the left (Account, Playback, Subtitles, Downloads, Interface, About),
// the selected category's options on the right. Values open a side sheet; toggles flip in place.
class SettingsSection : public SectionView {
public:
    explicit SettingsSection(MainMode mode);

    void onSectionShown(bool first) override;
    void onFocusWithin(ui::View* descendant) override;

private:
    enum class Category : int { Account, Playback, Subtitles, Downloads, Interface, About, Count };

    struct Option {
        std::string label;
        nlohmann::json value;
        std::string subtitle;
    };

    void showCategory(Category c, bool keepFocus = false);
    void refresh();
    std::vector<ui::ListItem> buildItems(Category c);
    ui::ListItem choice(const std::string& title, const std::string& key, std::vector<Option> options,
                        const nlohmann::json& fallback, std::function<void(const nlohmann::json&)> onChanged = {},
                        std::string subtitle = {});
    ui::ListItem toggle(const std::string& title, const std::string& key, bool fallback,
                        std::function<void(bool)> onChanged = {}, std::string subtitle = {});

    MainMode mode_;
    Category current_ = Category::Account;
    ui::ListMenu* categories_ = nullptr;
    ui::Label* paneTitle_ = nullptr;
    ui::Label* paneSubtitle_ = nullptr;
    ui::ListMenu* items_ = nullptr;
    size_t lastIndex_ = 0;      // right-pane row last activated
    bool restoreFocus_ = false; // put focus back on it after the next rebuild
};

}  // namespace karafin::screens
